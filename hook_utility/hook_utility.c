/*-------------------------------------------------------------------------
 *
 * hook_utility.c
 *		Use utility hook to restrict operations on a couple of DDL queries.
 *
 * Copyright (c) 1996-2020, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		hook_utility/hook_utility.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"
#include "miscadmin.h"
#include "tcop/utility.h"

PG_MODULE_MAGIC;

void _PG_init(void);
void _PG_fini(void);

/* Hardcoded values of parameters that interact with the hook */
static char *hook_dbname = "postres";
static char *hook_username = "postgres";
static ProcessUtility_hook_type prev_utility_hook = NULL;

static void dbrestrict_utility(PlannedStmt *pstmt,
							   const char *queryString,
							   ProcessUtilityContext context,
							   ParamListInfo params,
							   QueryEnvironment *queryEnv,
							   DestReceiver *dest,
							   char *completionTag);
static void load_params(void);

static
void dbrestrict_utility(PlannedStmt *pstmt,
						const char *queryString,
						ProcessUtilityContext context,
						ParamListInfo params,
						QueryEnvironment *queryEnv,
						DestReceiver *dest,
						char *completionTag)
{
	Node	   *parsetree = pstmt->utilityStmt;

	/* Do our custom process on drop database */
	switch (nodeTag(parsetree))
	{
		case T_DropdbStmt:
		{
			DropdbStmt *stmt = (DropdbStmt *) parsetree;
			char *username = GetUserNameFromId(GetUserId(), false);

			/*
			 * Check that only the authorized superuser foo can
			 * drop the database undroppable_foodb.
			 */
			if (strcmp(stmt->dbname, hook_dbname) == 0 &&
				strcmp(username, hook_username) != 0)
				ereport(ERROR,
						(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
						 errmsg("Only super-superuser \"%s\" can drop database \"%s\"",
								hook_username, hook_dbname)));
			break;
		}

		default:
			break;
	}

	/*
	 * Fallback to normal process, be it the previous hook loaded
	 * or the in-core code path if the previous hook does not exist.
	 */
	if (prev_utility_hook)
		(*prev_utility_hook) (pstmt, queryString,
							  context, params, queryEnv,
							  dest, completionTag);
	else
		standard_ProcessUtility(pstmt, queryString,
								context, params, queryEnv,
								dest, completionTag);
}

static
void load_params(void)
{
	/* Name of database that cannot be dropped by other users */
	DefineCustomStringVariable("hook_utility.dbname",
							   "Database on which DROP DATABASE is forbidden",
							   "Default value is \"postgres\".",
							   &hook_dbname,
							   "postgres",
							   PGC_POSTMASTER,
							   0, NULL, NULL, NULL);


	/* Name of use that cannot dropped the undropable database */
	DefineCustomStringVariable("hook_utility.username",
							   "User name able to do DROP DATABASE on given dbname",
							   "Default value is \"postgres\".",
							   &hook_username,
							   "postgres",
							   PGC_POSTMASTER,
							   0, NULL, NULL, NULL);

}

void
_PG_init(void)
{
	prev_utility_hook = ProcessUtility_hook;
	ProcessUtility_hook = dbrestrict_utility;

	/* Load custom parameters */
	load_params();
}
void
_PG_fini(void)
{
	ProcessUtility_hook = prev_utility_hook;
}
