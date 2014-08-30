/*-------------------------------------------------------------------------
 *
 * receiver_raw.c
 *		Receive and apply logical changes generated by decoder_raw. This
 *		creates some basics for a multi-master cluster using vanilla
 *		PostgreSQL without modifying its code.
 *
 * Copyright (c) 2013-2014, Michael Paquier
 * Copyright (c) 1996-2013, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		receiver_raw/receiver_raw.c
 *
 *-------------------------------------------------------------------------
 */

/* Some general headers for custom bgworker facility */
#include "postgres.h"
#include "fmgr.h"
#include "libpq-fe.h"
#include "pqexpbuffer.h"
#include "access/xact.h"
#include "lib/stringinfo.h"
#include "pgstat.h"
#include "executor/spi.h"
#include "postmaster/bgworker.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/proc.h"
#include "utils/guc.h"
#include "utils/snapmgr.h"

/* Allow load of this module in shared libs */
PG_MODULE_MAGIC;

/* Entry point of library loading */
void _PG_init(void);

/* Signal handling */
static volatile sig_atomic_t got_sigterm = false;
static volatile sig_atomic_t got_sighup = false;

/* GUC variables */
static char *receiver_database = "postgres";
static char *receiver_slot = "slot";
static char *receiver_conn_string = "replication=database dbname=postgres";
static int receiver_idle_time = 100;

/* Worker name */
static char *worker_name = "receiver_raw";

/* Lastly written positions */
static XLogRecPtr output_written_lsn = InvalidXLogRecPtr;
static XLogRecPtr output_fsync_lsn = InvalidXLogRecPtr;

/* Stream functions */
static void fe_sendint64(int64 i, char *buf);
static int64 fe_recvint64(char *buf);

static void
receiver_raw_sigterm(SIGNAL_ARGS)
{
	int save_errno = errno;
	got_sigterm = true;
	if (MyProc)
		SetLatch(&MyProc->procLatch);
	errno = save_errno;
}

static void
receiver_raw_sighup(SIGNAL_ARGS)
{
	int save_errno = errno;
	got_sighup = true;
	if (MyProc)
		SetLatch(&MyProc->procLatch);
	errno = save_errno;
}

/*
 * Send a Standby Status Update message to server.
 */
static bool
sendFeedback(PGconn *conn, int64 now)
{
	char		replybuf[1 + 8 + 8 + 8 + 8 + 1];
	int		 len = 0;

	ereport(LOG, (errmsg("%s: confirming write up to %X/%X, "
						 "flush to %X/%X (slot custom_slot)",
						 worker_name,
						 (uint32) (output_written_lsn >> 32),
						 (uint32) output_written_lsn,
						 (uint32) (output_fsync_lsn >> 32),
						 (uint32) output_fsync_lsn)));

	replybuf[len] = 'r';
	len += 1;
	fe_sendint64(output_written_lsn, &replybuf[len]);   /* write */
	len += 8;
	fe_sendint64(output_fsync_lsn, &replybuf[len]);	 /* flush */
	len += 8;
	fe_sendint64(InvalidXLogRecPtr, &replybuf[len]);	/* apply */
	len += 8;
	fe_sendint64(now, &replybuf[len]);  /* sendTime */
	len += 8;

	/* No reply requested from server */
	replybuf[len] = 0;
	len += 1;

	if (PQputCopyData(conn, replybuf, len) <= 0 || PQflush(conn))
	{
		ereport(LOG, (errmsg("%s: could not send feedback packet: %s",
							 worker_name, PQerrorMessage(conn))));
		return false;
	}

	return true;
}

/*
 * Converts an int64 to network byte order.
 */
static void
fe_sendint64(int64 i, char *buf)
{
	uint32	  n32;

	/* High order half first, since we're doing MSB-first */
	n32 = (uint32) (i >> 32);
	n32 = htonl(n32);
	memcpy(&buf[0], &n32, 4);

	/* Now the low order half */
	n32 = (uint32) i;
	n32 = htonl(n32);
	memcpy(&buf[4], &n32, 4);
}

/*
 * Converts an int64 from network byte order to native format.
 */
static int64
fe_recvint64(char *buf)
{
	int64	   result;
	uint32	  h32;
	uint32	  l32;

	memcpy(&h32, buf, 4);
	memcpy(&l32, buf + 4, 4);
	h32 = ntohl(h32);
	l32 = ntohl(l32);

	result = h32;
	result <<= 32;
	result |= l32;

	return result;
}

static int64
feGetCurrentTimestamp(void)
{
	int64	   result;
	struct timeval tp;

	gettimeofday(&tp, NULL);

	result = (int64) tp.tv_sec -
		((POSTGRES_EPOCH_JDATE - UNIX_EPOCH_JDATE) * SECS_PER_DAY);

	result = (result * USECS_PER_SEC) + tp.tv_usec;

	return result;
}

static void
feTimestampDifference(int64 start_time, int64 stop_time,
					  long *secs, int *microsecs)
{
	int64	   diff = stop_time - start_time;

	if (diff <= 0)
	{
		*secs = 0;
		*microsecs = 0;
	}
	else
	{
		*secs = (long) (diff / USECS_PER_SEC);
		*microsecs = (int) (diff % USECS_PER_SEC);
	}
}

static void
receiver_raw_main(Datum main_arg)
{
	/* Variables for replication connection */
	PQExpBuffer query;
	PGconn *conn;
	PGresult *res;

	/* Register functions for SIGTERM/SIGHUP management */
	pqsignal(SIGHUP, receiver_raw_sighup);
	pqsignal(SIGTERM, receiver_raw_sigterm);

	/* We're now ready to receive signals */
	BackgroundWorkerUnblockSignals();

	/* Connect to a database */
	BackgroundWorkerInitializeConnection(receiver_database, NULL);

	/* Establish connection to remote server */
	conn = PQconnectdb(receiver_conn_string);
	if (PQstatus(conn) != CONNECTION_OK)
	{
		PQfinish(conn);
		ereport(LOG, (errmsg("%s: Could not establish connection to remote server",
							 worker_name)));
		proc_exit(1);
	}

	/* Query buffer for remote connection */
	query = createPQExpBuffer();

	/* Start logical replication at specified position */
	appendPQExpBuffer(query, "START_REPLICATION SLOT \"%s\" LOGICAL 0/0 "
					         "(\"include_transaction\" 'off')",
					  receiver_slot);
	res = PQexec(conn, query->data);
	if (PQresultStatus(res) != PGRES_COPY_BOTH)
	{
		PQclear(res);
		ereport(LOG, (errmsg("%s: Could not start logical replication",
							 worker_name)));
		proc_exit(1);
	}
	PQclear(res);
	resetPQExpBuffer(query);

	while (!got_sigterm)
	{
		int rc, hdr_len;
		/* Buffer for COPY data */
		char	*copybuf = NULL;

		/* Wait necessary amount of time */
		rc = WaitLatch(&MyProc->procLatch,
					   WL_LATCH_SET | WL_TIMEOUT | WL_POSTMASTER_DEATH,
					   receiver_idle_time * 1L);
		ResetLatch(&MyProc->procLatch);

		/* Process signals */
		if (got_sighup)
		{
			/* Process config file */
			ProcessConfigFile(PGC_SIGHUP);
			got_sighup = false;
			ereport(LOG, (errmsg("%s: processed SIGHUP", worker_name)));
		}

		if (got_sigterm)
		{
			/* Simply exit */
			ereport(LOG, (errmsg("%s: processed SIGTERM", worker_name)));
			proc_exit(0);
		}

		/* Emergency bailout if postmaster has died */
		if (rc & WL_POSTMASTER_DEATH)
			proc_exit(1);

		/* Some cleanup */
		if (copybuf != NULL)
		{
			PQfreemem(copybuf);
			copybuf = NULL;
		}

		/*
		 * Begin a transaction before applying any changes. All the changes
		 * of the same batch are applied within the same transaction.
		 */
		SetCurrentStatementStartTimestamp();
		StartTransactionCommand();
		SPI_connect();
		PushActiveSnapshot(GetTransactionSnapshot());

		/*
		 * Receive data.
		 */
		while (true)
		{
			rc = PQgetCopyData(conn, &copybuf, 1);
			if (rc <= 0)
				break;

			/*
			 * Check message received from server:
			 * - 'k', keepalive message, bypass
			 * - 'w', check for streaming header
			 */
			if (copybuf[0] == 'k')
			{
				int		 pos;
				bool		replyRequested;
				XLogRecPtr  walEnd;

				/*
				 * Parse the keepalive message, enclosed in the CopyData message.
				 * We just check if the server requested a reply, and ignore the
				 * rest.
				 */
				pos = 1;	/* skip msgtype 'k' */
				walEnd = fe_recvint64(&copybuf[pos]);

				/*
				 * We mark here that the LSN received has already been flushed
				 * but this is actually incorrect. As a TODO item the feedback
				 * message should be sent once changes are correctly applied to
				 * the local database.
				 */
				output_written_lsn = Max(walEnd, output_written_lsn);
				output_fsync_lsn = output_written_lsn;
				pos += 8;	/* read walEnd */
				pos += 8;	/* skip sendTime */
				if (rc < pos + 1)
				{
					ereport(LOG, (errmsg("%s: streaming header too small: %d",
										 worker_name, rc)));
					proc_exit(1);
				}
				replyRequested = copybuf[pos];

				/*
				 * If the server requested an immediate reply, send one.
				 * TODO: send this confirmation only once change has been
				 * successfully written to the database here.
				 */
				if (replyRequested)
				{
					int64 now = feGetCurrentTimestamp();

					/* Leave is feedback is not sent properly */
					if (!sendFeedback(conn, now))
						proc_exit(1);
				}
				continue;
			}
			else if (copybuf[0] != 'w')
			{
				ereport(LOG, (errmsg("%s: Incorrect streaming header",
									 worker_name)));
				proc_exit(1);
			}

			/* Now fetch the data */
			hdr_len = 1;		/* msgtype 'w' */
			hdr_len += 8;		/* dataStart */
			hdr_len += 8;		/* walEnd */
			hdr_len += 8;		/* sendTime */
			if (rc < hdr_len + 1)
			{
				ereport(LOG, (errmsg("%s: Streaming header too small",
									 worker_name)));
				proc_exit(1);
			}

			/* Apply change to database */
			pgstat_report_activity(STATE_RUNNING, copybuf + hdr_len);
			SetCurrentStatementStartTimestamp();

			/* Execute query */
			rc = SPI_execute(copybuf + hdr_len, false, 0);

			if (rc == SPI_OK_INSERT)
				ereport(LOG, (errmsg("%s: INSERT received correctly: %s",
									 worker_name, copybuf + hdr_len)));
			else if (rc == SPI_OK_UPDATE)
				ereport(LOG, (errmsg("%s: UPDATE received correctly: %s",
									 worker_name, copybuf + hdr_len)));
			else if (rc == SPI_OK_DELETE)
				ereport(LOG, (errmsg("%s: DELETE received correctly: %s",
									 worker_name, copybuf + hdr_len)));
			else
				ereport(LOG, (errmsg("%s: Error when applying change: %s",
									 worker_name, copybuf + hdr_len)));

		}

		/* Finish process */
		SPI_finish();
		PopActiveSnapshot();
		CommitTransactionCommand();
		pgstat_report_activity(STATE_IDLE, NULL);

		/* No data, move to next loop */
		if (rc == 0)
		{
			/*
			 * In async mode, and no data available. We block on reading but
			 * not more than the specified timeout, so that we can send a
			 * response back to the client.
			 */
			int			r;
			fd_set	  input_mask;
			int64	   message_target = 0;
			int64	   fsync_target = 0;
			struct timeval timeout;
			struct timeval *timeoutptr = NULL;
			int64	   targettime;
			long		secs;
			int		 usecs;
			int64		now;

			FD_ZERO(&input_mask);
			FD_SET(PQsocket(conn), &input_mask);

			/* Now compute when to wakeup. */
			targettime = message_target;

			if (fsync_target > 0 && fsync_target < targettime)
				targettime = fsync_target;
			now = feGetCurrentTimestamp();
			feTimestampDifference(now,
								  targettime,
								  &secs,
								  &usecs);
			if (secs <= 0)
				timeout.tv_sec = 1; /* Always sleep at least 1 sec */
			else
				timeout.tv_sec = secs;
			timeout.tv_usec = usecs;
			timeoutptr = &timeout;

			r = select(PQsocket(conn) + 1, &input_mask, NULL, NULL, timeoutptr);
			if (r == 0 || (r < 0 && errno == EINTR))
			{
				/*
				 * Got a timeout or signal. Continue the loop and either
				 * deliver a status packet to the server or just go back into
				 * blocking.
				 */
				continue;
			}
			else if (r < 0)
			{
				ereport(LOG, (errmsg("%s: Incorrect status received... Leaving.",
									 worker_name)));
				proc_exit(1);
			}

			/* Else there is actually data on the socket */
			if (PQconsumeInput(conn) == 0)
			{
				ereport(LOG, (errmsg("%s: Data remaining on the socket... Leaving.",
									 worker_name)));
				proc_exit(1);
			}
			continue;
		}

		/* End of copy stream */
		if (rc == -1)
		{
			ereport(LOG, (errmsg("%s: COPY Stream has abruptly ended...",
								 worker_name)));
			break;
		}

		/* Failure when reading copy stream, leave */
		if (rc == -2)
		{
			ereport(LOG, (errmsg("%s: Failure while receiving changes...",
								 worker_name)));
			proc_exit(1);
		}
	}

	/* No problems, so clean exit */
	proc_exit(0);
}

/*
 * Entry point to load parameters
 */
static void
receiver_raw_load_params(void)
{
	/*
	 * Defines database where to connect and apply the changes.
	 */
	DefineCustomStringVariable("receiver_raw.database",
							   "Database where changes are applied.",
							   "Default value is \"postgres\".",
							   &receiver_database,
							   "postgres",
							   PGC_POSTMASTER,
							   0, NULL, NULL, NULL);

	/* Slot used to get replication changes on remote */
	DefineCustomStringVariable("receiver_raw.slot_name",
							   "Replication slot used for logical changes.",
							   "Default value is \"slot\".",
							   &receiver_slot,
							   "slot",
							   PGC_POSTMASTER,
							   0, NULL, NULL, NULL);

	/*
	 * Connection string used to connect to remote source.
	 * Note: This could be made far better as if user does not set
	 * replication START_REPLICATION will simply fail :)
	 */
	DefineCustomStringVariable("receiver_raw.conn_string",
							   "Replication slot used for logical changes.",
							   "Default value is \"slot\".",
							   &receiver_conn_string,
							   "replication=database dbname=postgres",
							   PGC_POSTMASTER,
							   0, NULL, NULL, NULL);

	/* Nap time between two loops */
	DefineCustomIntVariable("receiver_raw.idle_time",
							"Nap time between two successive loops (ms)",
							"Default value set to 100 ms.",
							&receiver_idle_time,
							100, 1, 10000,
							PGC_SIGHUP,
							0, NULL, NULL, NULL);
}

/*
 * Entry point for worker loading
 */
void
_PG_init(void)
{
	BackgroundWorker worker;

	receiver_raw_load_params();

	/* Worker parameter and registration */
	worker.bgw_flags = BGWORKER_SHMEM_ACCESS |
		BGWORKER_BACKEND_DATABASE_CONNECTION;
	worker.bgw_start_time = BgWorkerStart_ConsistentState;
	worker.bgw_main = receiver_raw_main;
	snprintf(worker.bgw_name, BGW_MAXLEN, "%s", worker_name);
	/* Wait 10 seconds for restart before crash */
	worker.bgw_restart_time = 10;
	worker.bgw_main_arg = (Datum) 0;
	worker.bgw_notify_pid = 0;
	RegisterBackgroundWorker(&worker);
}
