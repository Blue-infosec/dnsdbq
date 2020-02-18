#ifndef PDNS_H_INCLUDED
#define PDNS_H_INCLUDED 1

#include <jansson.h>
#include "netio.h"

struct pdns_json {
	json_t	*main,
		*time_first, *time_last, *zone_first, *zone_last,
		*bailiwick, *rrname, *rrtype, *rdata,
		*count, *num_results;
};

struct pdns_tuple {
	struct pdns_json  obj;
	u_long		  time_first, time_last, zone_first, zone_last;
	const char	 *bailiwick, *rrname, *rrtype, *rdata;
	json_int_t	  count, num_results;
};
typedef struct pdns_tuple *pdns_tuple_t;
typedef const struct pdns_tuple *pdns_tuple_ct;

struct pdns_system {
	const char	*name;
	const char	*base_url;
	char *		(*url)(const char *, char *);
	void		(*request_info)(void);
	int		(*info_blob)(const char *, size_t);
	void		(*auth)(reader_t);
	const char *	(*status)(reader_t);
	const char *	(*verb_ok)(const char *);
	const char *	(*setenv)(const char *, const char *);
	void		(*ready)(void);
	void		(*destroy)(void);
};
typedef const struct pdns_system *pdns_system_ct;

typedef void (*present_t)(pdns_tuple_ct, const char *, size_t, FILE *);

struct verb {
	const char	*name;
	const char	*url_fragment;
	void		(*ready)(void);
	present_t	text, json, csv;
};
typedef const struct verb *verb_ct;

typedef enum { no_mode = 0, rrset_mode, name_mode, ip_mode,
	       raw_rrset_mode, raw_name_mode } mode_e;

struct query {
	mode_e	mode;
	char	*thing;
	char	*rrtype;
	char	*bailiwick;
	char	*pfxlen;
	u_long	after;
	u_long	before;
};
typedef struct query *query_t;
typedef const struct query *query_ct;

void present_json(pdns_tuple_ct, const char *, size_t, FILE *);
void present_text_look(pdns_tuple_ct, const char *, size_t, FILE *);
void present_csv_look(pdns_tuple_ct, const char *, size_t, FILE *);
void present_text_summ(pdns_tuple_ct, const char *, size_t, FILE *);
void present_csv_summ(pdns_tuple_ct, const char *, size_t, FILE *);
const char *tuple_make(pdns_tuple_t, const char *, size_t);
void tuple_unmake(pdns_tuple_t);
int data_blob(writer_t, const char *, size_t);

#endif /*PDNS_H_INCLUDED*/