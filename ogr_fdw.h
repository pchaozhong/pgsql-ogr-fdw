/*-------------------------------------------------------------------------
 *
 * ogr_fdw.h
 *		  foreign-data wrapper for GIS data access.
 *
 * Copyright (c) 2014-2015, Paul Ramsey <pramsey@cleverelephant.ca>
 *
 *-------------------------------------------------------------------------
 */


/*
 * PostgreSQL
 */
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/reloptions.h"
#include "access/sysattr.h"
#include "access/transam.h"
#include "catalog/namespace.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_foreign_table.h"
#include "catalog/pg_foreign_server.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "commands/copy.h"
#include "commands/defrem.h"
#include "commands/explain.h"
#include "commands/vacuum.h"
#include "foreign/fdwapi.h"
#include "foreign/foreign.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/relation.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/pathnode.h"
#include "optimizer/planmain.h"
#include "optimizer/restrictinfo.h"
#include "optimizer/var.h"
#include "parser/parsetree.h"
#include "storage/ipc.h"
#include "utils/builtins.h"
#include "utils/date.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/numeric.h"
#include "utils/rel.h"
#include "utils/syscache.h"
#include "utils/timestamp.h"

/*
 * OGR library API
 */
#include "gdal.h"
#include "ogr_api.h"
#include "cpl_error.h"
#include "cpl_string.h"

/* Support for GDAL 1.X */
#if GDAL_VERSION_MAJOR < 2

/* Redefine variable types */
#define GDALDatasetH OGRDataSourceH
#define GDALDriverH OGRSFDriverH

/* Rename GDAL2 functions to OGR equivs */
#define GDALGetDriverCount() OGRGetDriverCount()
#define GDALAllRegister() OGRRegisterAll()
#define GDALGetDriverByName(name) OGRGetDriverByName(name)
#define GDALClose(ds) OGR_DS_Destroy(ds)
#define GDALDatasetGetLayerByName(ds,name) OGR_DS_GetLayerByName(ds,name)
#define GDALDatasetGetLayerCount(ds) OGR_DS_GetLayerCount(ds)
#define GDALDatasetGetLayer(ds,i) OGR_DS_GetLayer(ds,i)
#define GDALGetDriverShortName(dr) OGR_Dr_GetName(dr)
#define GDALGetDatasetDriver(ds) OGR_DS_GetDriver(ds)
	
#endif /* GDAL 1 support */

/* Utility macros for string equality */
#define streq(s1,s2) (strcmp((s1),(s2)) == 0)
#define strcaseeq(s1,s2) (strcasecmp((s1),(s2)) == 0)

typedef enum 
{
	OGR_UNMATCHED,
	OGR_GEOMETRY,
	OGR_FID,
	OGR_FIELD
} OgrColumnVariant;

typedef struct OgrFdwColumn
{
	/* PgSQL metadata */
	int pgattnum;            /* PostgreSQL attribute number */
	int pgattisdropped;      /* PostgreSQL attribute dropped? */
	char *pgname;            /* PostgreSQL column name */
	Oid pgtype;              /* PostgreSQL data type */
	int pgtypmod;            /* PostgreSQL type modifier */
	
	/* For reading */
	Oid pginputfunc;         /* PostgreSQL function to convert cstring to type */
	Oid pginputioparam;
	Oid pgrecvfunc;          /* PostgreSQL function to convert binary to type */
	Oid pgrecvioparam;
	
	/* For writing */
	Oid pgoutputfunc;        /* PostgreSQL function to convert type to cstring */
	bool pgoutputvarlena;
	Oid pgsendfunc;        /* PostgreSQL function to convert type to binary */
	bool pgsendvarlena;
	
	/* OGR metadata */
	OgrColumnVariant ogrvariant;
	int ogrfldnum;
	OGRFieldType ogrfldtype;

} OgrFdwColumn;

typedef struct OgrFdwTable
{
	int ncols;
	char *tblname;
	OgrFdwColumn *cols;
} OgrFdwTable;

typedef struct OgrConnection
{
	char *ds_str;         /* datasource connection string */
	char *dr_str;         /* driver (format) name */
	char *lyr_str;        /* layer name */
	char *config_options; /* GDAL config options */
	char *open_options;   /* GDAL open options */
	GDALDatasetH ds;
	OGRLayerH lyr;        /* OGR layer handle */
} OgrConnection;

typedef enum 
{
	OGR_PLAN_STATE,
	OGR_EXEC_STATE,
	OGR_MODIFY_STATE
} OgrFdwStateType;

typedef struct OgrFdwState
{
	OgrFdwStateType type;
	Oid foreigntableid; 
	OgrConnection ogr;
} OgrFdwState;

typedef struct OgrFdwPlanState
{
	OgrFdwStateType type;
	Oid foreigntableid; 
	OgrConnection ogr;   /* connection object */
	int nrows;           /* estimate of number of rows in file */
	Cost startup_cost; 
	Cost total_cost;
	bool *pushdown_clauses;
} OgrFdwPlanState;

typedef struct OgrFdwExecState
{
	OgrFdwStateType type;
	Oid foreigntableid; 
	OgrConnection ogr;     /* connection object */
	OgrFdwTable *table;
	TupleDesc tupdesc;
	char *sql;             /* OGR SQL for attribute filter */
	int rownum;            /* how many rows have we read thus far? */
	Oid setsridfunc;       /* ST_SetSRID() */
	Oid typmodsridfunc;    /* postgis_typmod_srid() */
} OgrFdwExecState;

typedef struct OgrFdwModifyState
{
	OgrFdwStateType type;
	Oid foreigntableid; 
	OgrConnection ogr;     /* connection object */
	OgrFdwTable *table;
	TupleDesc tupdesc;
} OgrFdwModifyState;

/* Shared function signatures */
bool ogrDeparse(StringInfo buf, PlannerInfo *root, RelOptInfo *foreignrel, List *exprs, List **param);

void ogrDeparseStringLiteral(StringInfo buf, const char *val);

/* Shared global value of the Geometry OId */
extern Oid GEOMETRYOID;

