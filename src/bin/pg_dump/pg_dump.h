/* -------------------------------------------------------------------------
 *
 * pg_dump.h
 *	  Common header file for the pg_dump utility
 *
 * Portions Copyright (c) 1996-2012, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/bin/pg_dump/pg_dump.h
 *
 * -------------------------------------------------------------------------
 */

#ifndef PG_DUMP_H
#define PG_DUMP_H

#include "postgres_fe.h"

/*
 * pg_dump uses two different mechanisms for identifying database objects:
 *
 * CatalogId represents an object by the tableoid and oid of its defining
 * entry in the system catalogs.  We need this to interpret pg_depend entries,
 * for instance.
 *
 * DumpId is a simple sequential integer counter assigned as dumpable objects
 * are identified during a pg_dump run.  We use DumpId internally in preference
 * to CatalogId for two reasons: it's more compact, and we can assign DumpIds
 * to "objects" that don't have a separate CatalogId.  For example, it is
 * convenient to consider a table, its data, and its ACL as three separate
 * dumpable "objects" with distinct DumpIds --- this lets us reason about the
 * order in which to dump these things.
 */
typedef struct {
    Oid tableoid;
    Oid oid;
} CatalogId;

typedef int DumpId;

/*
 * The data structures used to store system catalog information.  Every
 * dumpable object is a subclass of DumpableObject.
 *
 * NOTE: the structures described here live for the entire pg_dump run;
 * and in most cases we make a struct for every object we can find in the
 * catalogs, not only those we are actually going to dump.	Hence, it's
 * best to store a minimal amount of per-object info in these structs,
 * and retrieve additional per-object info when and if we dump a specific
 * object.	In particular, try to avoid retrieving expensive-to-compute
 * information until it's known to be needed.  We do, however, have to
 * store enough info to determine whether an object should be dumped and
 * what order to dump in.
 */
typedef enum {
    /* When modifying this enum, update priority tables in pg_dump_sort.c! */
    DO_NAMESPACE,
    DO_EXTENSION,
    DO_TYPE,
    DO_SHELL_TYPE,
    DO_FUNC,
    DO_AGG,
    DO_OPERATOR,
    DO_OPCLASS,
    DO_OPFAMILY,
    DO_COLLATION,
    DO_CONVERSION,
    DO_TABLE,
    DO_ATTRDEF,
    DO_INDEX,
    DO_RULE,
    DO_TRIGGER,
    DO_CONSTRAINT,
    DO_FK_CONSTRAINT, /* see note for ConstraintInfo */
    DO_PROCLANG,
    DO_CAST,
    DO_PACKAGE,
    DO_TABLE_DATA,
    DO_DUMMY_TYPE,
    DO_TSPARSER,
    DO_TSDICT,
    DO_TSTEMPLATE,
    DO_TSCONFIG,
    DO_FDW,
    DO_FOREIGN_SERVER,
    DO_DEFAULT_ACL,
    DO_BLOB,
    DO_BLOB_DATA,
    DO_PRE_DATA_BOUNDARY,
    DO_POST_DATA_BOUNDARY,
    DO_FTBL_CONSTRAINT, /* dump informational constraint info of the HDFS foreign table, also used for MOT table */
    DO_RLSPOLICY,       /* dump row level security policy of table */
    DO_PUBLICATION,
    DO_PUBLICATION_REL,
    DO_SUBSCRIPTION,
    DO_EVENT,
    DO_EVENT_TRIGGER
} DumpableObjectType;

typedef struct _dumpableObject {
    DumpableObjectType objType;
    CatalogId catId;                /* zero if not a cataloged object */
    DumpId dumpId;                  /* assigned by AssignDumpId() */
    char* name;                     /* object name (should never be NULL) */
    struct _namespaceInfo* nmspace; /* containing nmspace, or NULL */
    bool dump;                      /* true if we want to dump this object */
    bool ext_member;                /* true if object is member of extension */
    DumpId* dependencies;           /* dumpIds of objects this one depends on */
    int nDeps;                      /* number of valid dependencies */
    int allocDeps;                  /* allocated size of dependencies[] */
} DumpableObject;

typedef struct _evttriggerInfo {
    DumpableObject dobj;
    char       *evtname;
    char       *evtevent;
    char       *evtowner;
    char       *evttags;
    char       *evtfname;
    char        evtenabled;
} EventTriggerInfo;

typedef struct _namespaceInfo {
    DumpableObject dobj;
    char* rolname; /* name of owner, or empty string */
    char* nspacl;
    bool hasBlockchain;
    int collate;
} NamespaceInfo;

typedef struct _extensionInfo {
    DumpableObject dobj;
    char* nmspace; /* schema containing extension's objects */
    bool relocatable;
    char* extversion;
    char* extconfig; /* info about configuration tables */
    char* extcondition;
    char* extrole;
} ExtensionInfo;

typedef struct _typeInfo {
    DumpableObject dobj;

    /*
     * Note: dobj.name is the pg_type.typname entry.  format_type() might
     * produce something different than typname
     */
    char* rolname; /* name of owner, or empty string */
    char* typacl;
    Oid typelem;
    Oid typrelid;
    char typrelkind; /* 'r', 'v', 'c', etc */
    char typtype;    /* 'b', 'c', etc */
    bool isArray;    /* true if auto-generated array type */
    bool isDefined;  /* true if typisdefined */
    /* If it's a dumpable base type, we create a "shell type" entry for it */
    struct _shellTypeInfo* shellType; /* shell-type entry, or NULL */
    /* If it's a domain, we store links to its constraints here: */
    int nDomChecks;
    struct _constraintInfo* domChecks;
} TypeInfo;

typedef struct _shellTypeInfo {
    DumpableObject dobj;

    TypeInfo* baseType; /* back link to associated base type */
} ShellTypeInfo;

typedef struct _funcInfo {
    DumpableObject dobj;
    char* rolname; /* name of owner, or empty string */
    Oid lang;
    int nargs;
    Oid* argtypes;
    Oid prorettype;
    char* proacl;
} FuncInfo;


typedef struct _pkgInfo {
    DumpableObject dobj;
    char* rolname; /* name of owner, or empty string */
    char* pkgacl;
} PkgInfo;

/* AggInfo is a superset of FuncInfo */
typedef struct _aggInfo {
    FuncInfo aggfn;
    /* we don't require any other fields at the moment */
} AggInfo;

typedef struct _oprInfo {
    DumpableObject dobj;
    char* rolname;
    char oprkind;
    Oid oprcode;
} OprInfo;

typedef struct _opclassInfo {
    DumpableObject dobj;
    char* rolname;
} OpclassInfo;

typedef struct _opfamilyInfo {
    DumpableObject dobj;
    char* rolname;
} OpfamilyInfo;

typedef struct _collInfo {
    DumpableObject dobj;
    char* rolname;
} CollInfo;

typedef struct _convInfo {
    DumpableObject dobj;
    char* rolname;
} ConvInfo;

typedef struct _tableInfo {
    /*
     * These fields are collected for every table in the database.
     */
    DumpableObject dobj;
    char* rolname; /* name of owner, or empty string */
    char* relacl;
    char relkind;
    int1 relcmprs;            /* row compression attribution */
    char relpersistence;      /* relation persistence */
    char relreplident;        /* replica identifier */
    char* reltablespace;      /* relation tablespace */
    char* reloptions;         /* options specified by WITH (...) */
    char* checkoption;        /* WITH CHECK OPTION */
    Oid   relbucket;          /* relation bucket OID */	    
    char* toast_reloptions;   /* ditto, for the TOAST table */
    bool hasindex;            /* does it have any indexes? */
    bool hasrules;            /* does it have any rules? */
    bool hastriggers;         /* does it have any triggers? */
    bool hasoids;             /* does it have OIDs? */
    bool isMOT;               /* true if it is MOT table */
    bool isblockchain;        /* is it in blockchain schema? */
    uint32 frozenxid;         /* for restore frozen xid */
    uint64 frozenxid64;       /* for restore frozen xid64 */
    Oid toast_oid;            /* for restore toast frozen xid */
    uint32 toast_frozenxid;   /* for restore toast frozen xid */
    uint64 toast_frozenxid64; /* for restore toast frozen xid */
    int ncheck;               /* # of CHECK expressions */
    char* reloftype;          /* underlying type for typed table */
    /* these two are set only if table is a sequence owned by a column: */
    Oid owning_tab; /* OID of table owning sequence */
    int owning_col; /* attr # of column owning sequence */
    char parttype;
    bool relrowmovement;
    bool isIncremental; /* for matview, true if is an incremental type */

    bool interesting; /* true if need to collect more data */
    int autoinc_attnum;
    DumpId autoincconstraint;
    DumpId autoincindex;
    char* autoinc_seqname;
    char* viewsecurity;
#ifdef PGXC
    /* PGXC table locator Data */
    char pgxclocatortype;  /* Type of PGXC table locator */
    char* pgxcattnum;      /* Number of the attribute the table is partitioned with */
    char* pgxc_node_names; /* List of node names where this table is distributed */
#endif
    /*
     * These fields are computed only if we decide the table is interesting
     * (it's either a table to dump, or a direct parent of a dumpable table).
     */
    int numatts;                        /* number of attributes */
    char** attnames;                    /* the attribute names */
    char** column_key_names;            /* column key names */
    int* encryption_type;               /* encryption type */
    char** atttypnames;                 /* attribute type names */
    int* typid;                         /* attribute type oid */
    int* atttypmod;                     /* type-specific type modifiers */
    int* attstattarget;                 /* attribute statistics targets */
    char* attstorage;                   /* attribute storage scheme */
    char* typstorage;                   /* type storage scheme */
    bool* attisdropped;                 /* true if attr is dropped; don't dump it */
    bool* attisblockchainhash;          /* true if attr is "hash" column in blockchain table */
    int* attlen;                        /* attribute length, used by binary_upgrade */
    char* attalign;                     /* attribute align, used by binary_upgrade */
    bool* attislocal;                   /* true if attr has local definition */
    char** attoptions;                  /* per-attribute options */
    Oid* attcollation;                  /* per-attribute collation selection */
    char** attfdwoptions;               /* per-attribute fdw options */
    bool* notnull;                      /* NOT NULL constraints on attributes */
    bool* inhNotNull;                   /* true if NOT NULL is inherited */
    int* attkvtype;                     /* will not be 0 in timeseries table other wise 0 */
    struct _attrDefInfo** attrdefs;     /* DEFAULT expressions */
    struct _constraintInfo* checkexprs; /* CHECK constraints */

    /*
     * Stuff computed only for dumpable tables.
     */
    int numParents;                     /* number of (immediate) parent tables */
    struct _tableInfo** parents;        /* TableInfos of immediate parents */
    struct _tableDataInfo* dataObj;     /* TableDataInfo, if dumping its data */
} TableInfo;

typedef struct _attrDefInfo {
    DumpableObject dobj; /* note: dobj.name is name of table */
    TableInfo* adtable;  /* link to table of attribute */
    int adnum;
    char* adef_expr;     /* decompiled DEFAULT expression */
    bool separate;       /* TRUE if must dump as separate item */
    char generatedCol;
    char* adupd_expr;    /* on update expression of on update timestamp syntax on Mysql dbcompatibility */
} AttrDefInfo;

typedef struct _tableDataInfo {
    DumpableObject dobj;
    TableInfo* tdtable; /* link to table to dump */
    bool oids;          /* include OIDs in data? */
    char* filtercond;   /* WHERE condition to limit rows dumped */
} TableDataInfo;

typedef struct _indxInfo {
    DumpableObject dobj;
    TableInfo* indextable; /* link to table the index is for */
    char* indexdef;
    char* tablespace;      /* tablespace in which index is stored */
    char* options;         /* options specified by WITH (...) */
    int indnkeys;
    int indnkeyattrs;	   /* number of index key attributes */
    int	indnattrs;         /* total number of index attributes */
    Oid* indkeys;          /* In spite of the name 'indkeys' this field
                            * contains both key and nonkey attributes */
    bool indisclustered;
    bool indisusable;
    bool indisreplident;
    bool indisvisible;
    /* if there is an associated constraint object, its dumpId: */
    DumpId indexconstraint;
} IndxInfo;

typedef struct _ruleInfo {
    DumpableObject dobj;
    TableInfo* ruletable; /* link to table the rule is for */
    char ev_type;
    bool is_instead;
    char ev_enabled;
    bool separate; /* TRUE if must dump as separate item */
    /* separate is always true for non-ON SELECT rules */
    char* reloptions; /* options specified by WITH (...) */
                      /* reloptions is only set if we need to dump the options with the rule */
} RuleInfo;

/* Row Level Security Information */
typedef struct _rlsPolicyInfo {
    DumpableObject dobj;
    TableInfo* policytable; /* link to table that RLS policy is for */
    char polcmd;            /* same with pg_rlspolicy.polcmd */
    bool polpermissive;     /* same with pg_rlspolicy.polpermissive */
    char* polname;          /* row level security policy name */
    char* polroles;         /* role list that RLS policy applies to */
    char* polqual;          /* policy qual defination */
} RlsPolicyInfo;

typedef struct _triggerInfo {
    DumpableObject dobj;
    TableInfo* tgtable; /* link to table the trigger is for */
    char* tgfname;
    int tgtype;
    int tgnargs;
    char* tgargs;
    bool tgisconstraint;
    char* tgconstrname;
    Oid tgconstrrelid;
    char* tgconstrrelname;
    char tgenabled;
    bool tgdeferrable;
    bool tginitdeferred;
    char* tgdef;
    bool tgdb;
    bool tgbodybstyle;
} TriggerInfo;
typedef struct _eventInfo {
    DumpableObject dobj;
    char* evdefiner;
    char* evname;
    char* nspname;
    char* starttime;
    char* endtime;
    char* intervaltime;
    bool autodrop;
    bool evstatus;
    char* comment;
    char* evbody;
}EventInfo;

/*
 * struct ConstraintInfo is used for all constraint types.	However we
 * use a different objType for foreign key constraints, to make it easier
 * to sort them the way we want.
 *
 * Note: condeferrable and condeferred are currently only valid for
 * unique/primary-key constraints.	Otherwise that info is in condef.
 */
typedef struct _constraintInfo {
    DumpableObject dobj;
    TableInfo* contable; /* NULL if domain constraint */
    TypeInfo* condomain; /* NULL if table constraint */
    char contype;
    char* condef;       /* definition, if CHECK or FOREIGN KEY */
    Oid confrelid;      /* referenced table, if FOREIGN KEY */
    DumpId conindex;    /* identifies associated index if any */
    bool condeferrable; /* TRUE if constraint is DEFERRABLE */
    bool condeferred;   /* TRUE if constraint is INITIALLY DEFERRED */
    bool conislocal;    /* TRUE if constraint has local definition */
    bool separate;      /* TRUE if must dump as separate item */
    int conkey;         /* Record the number of column with constraint */
    bool consoft;       /* TRUE if the constraint is informational constraint */
    bool conopt;        /* TRUE if the informational constraint has "enble query optimization" attribute */
} ConstraintInfo;

typedef struct _procLangInfo {
    DumpableObject dobj;
    bool lanpltrusted;
    Oid lanplcallfoid;
    Oid laninline;
    Oid lanvalidator;
    char* lanacl;
    char* rolename; /* name of owner, or empty string */
} ProcLangInfo;

typedef struct _castInfo {
    DumpableObject dobj;
    Oid castsource;
    Oid casttarget;
    Oid castfunc;
    char castcontext;
    char castmethod;
} CastInfo;

/* InhInfo isn't a DumpableObject, just temporary state */
typedef struct _inhInfo {
    Oid inhrelid;  /* OID of a child table */
    Oid inhparent; /* OID of its parent */
} InhInfo;

typedef struct _prsInfo {
    DumpableObject dobj;
    Oid prsstart;
    Oid prstoken;
    Oid prsend;
    Oid prsheadline;
    Oid prslextype;
} TSParserInfo;

typedef struct _dictInfo {
    DumpableObject dobj;
    char* rolname;
    Oid dicttemplate;
    char* dictinitoption;
} TSDictInfo;

typedef struct _tmplInfo {
    DumpableObject dobj;
    Oid tmplinit;
    Oid tmpllexize;
} TSTemplateInfo;

typedef struct _cfgInfo {
    DumpableObject dobj;
    char* rolname;
    Oid cfgparser;
} TSConfigInfo;

typedef struct _fdwInfo {
    DumpableObject dobj;
    char* rolname;
    char* fdwhandler;
    char* fdwvalidator;
    char* fdwoptions;
    char* fdwacl;
} FdwInfo;

typedef struct _foreignServerInfo {
    DumpableObject dobj;
    char* rolname;
    Oid srvfdw;
    char* srvtype;
    char* srvversion;
    char* srvacl;
    char* srvoptions;
} ForeignServerInfo;

typedef struct _defaultACLInfo {
    DumpableObject dobj;
    char* defaclrole;
    char defaclobjtype;
    char* defaclacl;
} DefaultACLInfo;

typedef struct _blobInfo {
    DumpableObject dobj;
    char* rolname;
    char* blobacl;
} BlobInfo;

/*
 * The PublicationInfo struct is used to represent publications.
 */
typedef struct _PublicationInfo {
    DumpableObject dobj;
    char *rolname;
    bool puballtables;
    bool pubinsert;
    bool pubupdate;
    bool pubdelete;
} PublicationInfo;

/*
 * The PublicationRelInfo struct is used to represent publication table
 * mapping.
 */
typedef struct _PublicationRelInfo {
    DumpableObject dobj;
    TableInfo *pubtable;
    char *pubname;
} PublicationRelInfo;

/*
 * The SubscriptionInfo struct is used to represent subscription.
 */
typedef struct _SubscriptionInfo {
    DumpableObject dobj;
    char *rolname;
    char *subconninfo;
    char *subslotname;
    char *subsynccommit;
    char *subpublications;
    char *subbinary;
} SubscriptionInfo;

/* global decls */
extern bool force_quotes; /* double-quotes for identifiers flag */
extern bool g_verbose;    /* verbose flag */

/* placeholders for comment starting and ending delimiters */
extern char g_comment_start[10];
extern char g_comment_end[10];

extern char g_opaque_type[10]; /* name for the opaque type */

/*
 *	common utility functions
 */
struct Archive;
typedef struct Archive Archive;

extern TableInfo* getSchemaData(Archive*, int* numTablesPtr);

typedef enum _OidOptions { zeroAsOpaque = 1, zeroAsAny = 2, zeroAsStar = 4, zeroAsNone = 8 } OidOptions;

extern void AssignDumpId(DumpableObject* dobj);
extern DumpId createDumpId(void);
extern DumpId getMaxDumpId(void);
extern DumpableObject* findObjectByDumpId(DumpId dumpId);
extern DumpableObject* findObjectByCatalogId(CatalogId catalogId);
extern void getDumpableObjects(DumpableObject*** objs, int* numObjs);

extern void addObjectDependency(DumpableObject* dobj, DumpId refId);
extern void removeObjectDependency(DumpableObject* dobj, DumpId refId);

extern TableInfo* findTableByOid(Oid oid);
extern TypeInfo* findTypeByOid(Oid oid);
extern FuncInfo* findFuncByOid(Oid oid);
extern OprInfo* findOprByOid(Oid oid);
extern CollInfo* findCollationByOid(Oid oid);
extern NamespaceInfo* findNamespaceByOid(Oid oid);

extern void parseOidArray(const char* str, Oid* array, int arraysize);

extern void sortDumpableObjects(DumpableObject** objs, int numObjs, DumpId preBoundaryId, DumpId postBoundaryId);
extern void sortDumpableObjectsByTypeName(DumpableObject** objs, int numObjs);
extern void sortDumpableObjectsByTypeOid(DumpableObject** objs, int numObjs);

/*
 * version specific routines
 */
extern NamespaceInfo* getNamespaces(Archive* fout, int* numNamespaces);
extern ExtensionInfo* getExtensions(Archive* fout, int* numExtensions);
extern TypeInfo* getTypes(Archive* fout, int* numTypes);
extern FuncInfo* getFuncs(Archive* fout, int* numFuncs);
extern PkgInfo* getPackages(Archive* fout, int* numPackages); 
extern AggInfo* getAggregates(Archive* fout, int* numAggregates);
extern OprInfo* getOperators(Archive* fout, int* numOperators);
extern OpclassInfo* getOpclasses(Archive* fout, int* numOpclasses);
extern OpfamilyInfo* getOpfamilies(Archive* fout, int* numOpfamilies);
extern CollInfo* getCollations(Archive* fout, int* numCollations);
extern ConvInfo* getConversions(Archive* fout, int* numConversions);
extern TableInfo* getTables(Archive* fout, int* numTables);
extern void getOwnedSeqs(Archive* fout, TableInfo tblinfo[], int numTables);
extern InhInfo* getInherits(Archive* fout, int* numInherits);
extern void getIndexes(Archive* fout, TableInfo tblinfo[], int numTables);
extern void getConstraintsOnForeignTable(Archive* fout, TableInfo tblinfo[], int numTables);
extern void getConstraints(Archive* fout, TableInfo tblinfo[], int numTables);
extern RuleInfo* getRules(Archive* fout, int* numRules);
extern void getRlsPolicies(Archive* fout, TableInfo tblinfo[], int numTables);
extern void getTriggers(Archive* fout, TableInfo tblinfo[], int numTables);
extern EventInfo *getEvents(Archive *fout, int *numEvents);
extern ProcLangInfo* getProcLangs(Archive* fout, int* numProcLangs);
extern CastInfo* getCasts(Archive* fout, int* numCasts);
extern void getTableAttrs(Archive* fout, TableInfo* tbinfo, int numTables);
extern bool shouldPrintColumn(TableInfo* tbinfo, int colno);
extern TSParserInfo* getTSParsers(Archive* fout, int* numTSParsers);
extern TSDictInfo* getTSDictionaries(Archive* fout, int* numTSDicts);
extern TSTemplateInfo* getTSTemplates(Archive* fout, int* numTSTemplates);
extern TSConfigInfo* getTSConfigurations(Archive* fout, int* numTSConfigs);
extern FdwInfo* getForeignDataWrappers(Archive* fout, int* numForeignDataWrappers);
extern ForeignServerInfo* getForeignServers(Archive* fout, int* numForeignServers);
extern DefaultACLInfo* getDefaultACLs(Archive* fout, int* numDefaultACLs);
extern void getExtensionMembership(Archive* fout, ExtensionInfo extinfo[], int numExtensions);
extern void help(const char* progname);
extern bool IsRbObject(Archive* fout, Oid classid, Oid objid, const char* objname);
extern uint32 GetVersionNum(Archive* fout);
extern void getPublications(Archive *fout);
extern void getPublicationTables(Archive *fout, TableInfo tblinfo[], int numTables);
extern void getSubscriptions(Archive *fout);
extern EventTriggerInfo *getEventTriggers(Archive *fout, int *numEventTriggers);
extern bool TabExists(Archive* fout, const char* schemaName, const char* tabName);

#ifdef GSDUMP_LLT
void stopLLT();
#endif
#endif /* PG_DUMP_H */
