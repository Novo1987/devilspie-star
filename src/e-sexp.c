/* 
 * Copyright 2000 Ximian (www.ximian.com).
 *
 * A simple, extensible s-exp evaluation engine.
 *
 * Author : 
 *  Michael Zucchi <notzed@ximian.com>
 *
 * This program is free software; you can redistribute it and/or 
 * modify it under the terms of version 2 of the GNU Lesser General Public 
 * License as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
 * USA
 */

/*
  The following built-in s-exp's are supported:

  "string"
	A string.
  
  1, 2, 3
	Integers

  #t, #f
	Booleans.

  list = (and list*)
	perform an intersection of a number of lists, and return that.

  bool = (and bool*)
	perform a boolean AND of boolean values.

  list = (or list*)
	perform a union of a number of lists, returning the new list.

  bool = (or bool*)
	perform a boolean OR of boolean values.

  int = (+ int*)
	Add integers.

  string = (+ string*)
	Concat strings.

  time_t = (+ time_t*)
	Add time_t values.

  int = (- int int*)
	Subtract integers from the first.

  time_t = (- time_t*)
	Subtract time_t values from the first.

  int = (cast-int string|int|bool)
        Cast to an integer value.

  string = (cast-string string|int|bool)
        Cast to an string value.

  Comparison operators:

  bool = (< int int)
  bool = (> int int)
  bool = (= int int)

  bool = (< string string)
  bool = (> string string)
  bool = (= string string)

  bool = (< time_t time_t)
  bool = (> time_t time_t)
  bool = (= time_t time_t)
	Perform a comparision of 2 integers, 2 string values, or 2 time values.

  Function flow:

  type = (if bool function)
  type = (if bool function function)
  	Choose a flow path based on a boolean value

  type = (begin  func func func)
        Execute a sequence.  The last function return is the return type.
*/


#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdlib.h>
#include <time.h>
#include <string.h>

#include <glib-object.h>
#include <glib/gi18n.h>

#include "e-sexp.h"

static ESExpTerm * parse_list(ESExp *f, int gotbrace);
static ESExpTerm * parse_value(ESExp *f);

static void parse_dump_term(ESExpTerm *t, int depth);

static GObjectClass *parent_class = NULL;

static const GScannerConfig scanner_config =
{
	( " \t\r\n")		/* cset_skip_characters */,
	( G_CSET_a_2_z
	  "_+-<=>?"
	  G_CSET_A_2_Z)		/* cset_identifier_first */,
	( G_CSET_a_2_z
	  "_0123456789-<>?"
	  G_CSET_A_2_Z
	  G_CSET_LATINS
	  G_CSET_LATINC	)	/* cset_identifier_nth */,
	( ";\n" )		/* cpair_comment_single */,
  
	FALSE			/* case_sensitive */,
  
	TRUE			/* skip_comment_multi */,
	TRUE			/* skip_comment_single */,
	TRUE			/* scan_comment_multi */,
	TRUE			/* scan_identifier */,
	TRUE			/* scan_identifier_1char */,
	FALSE			/* scan_identifier_NULL */,
	TRUE			/* scan_symbols */,
	FALSE			/* scan_binary */,
	TRUE			/* scan_octal */,
	TRUE			/* scan_float */,
	TRUE			/* scan_hex */,
	FALSE			/* scan_hex_dollar */,
	TRUE			/* scan_string_sq */,
	TRUE			/* scan_string_dq */,
	TRUE			/* numbers_2_int */,
	FALSE			/* int_2_float */,
	FALSE			/* identifier_2_string */,
	TRUE			/* char_2_token */,
	FALSE			/* symbol_2_token */,
	FALSE			/* scope_0_fallback */,
};

/* jumps back to the caller of f->failenv, only to be called from inside a callback */
void
e_sexp_fatal_error(ESExp *f, char *why, ...)
{
	va_list args;

	if (f->error)
		g_free(f->error);
	
	va_start(args, why);
	f->error = g_strdup_vprintf(why, args);
	va_end(args);

	longjmp(f->failenv, 1);
}

const char *
e_sexp_error(ESExp *f)
{
	return f->error;
}

ESExpResult *
e_sexp_result_new	(ESExp *f, ESExpResultType type)
{
	ESExpResult *r = g_slice_new0 (ESExpResult);
	r->type = type;
	return r;
}

ESExpResult *
e_sexp_result_new_bool(ESExp *f, gboolean b)
{
  ESExpResult *r = e_sexp_result_new (f, ESEXP_RES_BOOL);
  r->value.bool = b;
  return r;
}

void
e_sexp_result_free(ESExp *f, ESExpResult *t)
{
	if (t == NULL)
		return;

	switch(t->type) {
	case ESEXP_RES_ARRAY_PTR:
		g_ptr_array_free(t->value.ptrarray, TRUE);
		break;
	case ESEXP_RES_BOOL:
	case ESEXP_RES_INT:
	case ESEXP_RES_TIME:
		break;
	case ESEXP_RES_STRING:
		g_free(t->value.string);
		break;
	case ESEXP_RES_UNDEFINED:
		break;
	default:
		g_assert_not_reached();
	}
	g_slice_free (ESExpResult, t);
}

/* used in normal functions if they have to abort, and free their arguments */
void
e_sexp_resultv_free(ESExp *f, int argc, ESExpResult **argv)
{
	int i;

	for (i=0;i<argc;i++) {
		e_sexp_result_free(f, argv[i]);
	}
}

/* implementations for the builtin functions */

/* can you tell, i dont like glib? */
/* we can only itereate a hashtable from a called function */
struct _glib_sux_donkeys {
	int count;
	GPtrArray *uids;
};

/* ok, store any values that are in all sets */
static void
g_lib_sux_htand(char *key, int value, struct _glib_sux_donkeys *fuckup)
{
	if (value == fuckup->count) {
		g_ptr_array_add(fuckup->uids, key);
	}
}

/* or, store all unique values */
static void
g_lib_sux_htor(char *key, int value, struct _glib_sux_donkeys *fuckup)
{
	g_ptr_array_add(fuckup->uids, key);
}

static ESExpResult *
term_eval_and(ESExp *f, int argc, ESExpTerm **argv, void *data)
{
	ESExpResult *r, *r1;
	GHashTable *ht = g_hash_table_new(g_str_hash, g_str_equal);
	struct _glib_sux_donkeys lambdafoo;
	int type=-1;
	int bool = TRUE;
	int i;
	
	r = e_sexp_result_new(f, ESEXP_RES_UNDEFINED);
	
	for (i=0;bool && i<argc;i++) {
		r1 = e_sexp_term_eval(f, argv[i]);
		if (type == -1)
			type = r1->type;
		if (type != r1->type) {
			e_sexp_result_free(f, r);
			e_sexp_result_free(f, r1);
			g_hash_table_destroy(ht);
			e_sexp_fatal_error(f, _("Invalid types in AND"));
		} else if (r1->type == ESEXP_RES_ARRAY_PTR) {
			char **a1;
			int l1, j;
			
			a1 = (char **)r1->value.ptrarray->pdata;
			l1 = r1->value.ptrarray->len;
			for (j=0;j<l1;j++) {
			        gpointer ptr;
				int n;
				ptr = g_hash_table_lookup(ht, a1[j]);
				n = GPOINTER_TO_INT(ptr);
				g_hash_table_insert(ht, a1[j], GINT_TO_POINTER(n+1));
			}
		} else if (r1->type == ESEXP_RES_BOOL) {
			bool = bool && r1->value.bool;
		}
		e_sexp_result_free(f, r1);
	}
	
	if (type == ESEXP_RES_ARRAY_PTR) {
		lambdafoo.count = argc;
		lambdafoo.uids = g_ptr_array_new();
		g_hash_table_foreach(ht, (GHFunc)g_lib_sux_htand, &lambdafoo);
		r->type = ESEXP_RES_ARRAY_PTR;
		r->value.ptrarray = lambdafoo.uids;
	} else if (type == ESEXP_RES_BOOL) {
		r->type = ESEXP_RES_BOOL;
		r->value.bool = bool;
	}
	
	g_hash_table_destroy(ht);
	
	return r;
}

static ESExpResult *
term_eval_or(ESExp *f, int argc, ESExpTerm **argv, void *data)
{
	ESExpResult *r, *r1;
	GHashTable *ht = g_hash_table_new(g_str_hash, g_str_equal);
	struct _glib_sux_donkeys lambdafoo;
	int type = -1;
	int bool = FALSE;
	int i;
	
	r = e_sexp_result_new(f, ESEXP_RES_UNDEFINED);
	
	for (i=0;!bool && i<argc;i++) {
		r1 = e_sexp_term_eval(f, argv[i]);
		if (type == -1)
			type = r1->type;
		if (r1->type != type) {
			e_sexp_result_free(f, r);
			e_sexp_result_free(f, r1);
			g_hash_table_destroy(ht);
			e_sexp_fatal_error(f, _("Invalid types in OR"));
		} else if (r1->type == ESEXP_RES_ARRAY_PTR) {
			char **a1;
			int l1, j;
			
			a1 = (char **)r1->value.ptrarray->pdata;
			l1 = r1->value.ptrarray->len;
			for (j=0;j<l1;j++) {
				g_hash_table_insert(ht, a1[j], (void *)1);
			}
		} else if (r1->type == ESEXP_RES_BOOL) {
			bool |= r1->value.bool;				
		}
		e_sexp_result_free(f, r1);
	}
	
	if (type == ESEXP_RES_ARRAY_PTR) {
		lambdafoo.count = argc;
		lambdafoo.uids = g_ptr_array_new();
		g_hash_table_foreach(ht, (GHFunc)g_lib_sux_htor, &lambdafoo);
		r->type = ESEXP_RES_ARRAY_PTR;
		r->value.ptrarray = lambdafoo.uids;
	} else if (type == ESEXP_RES_BOOL) {
		r->type = ESEXP_RES_BOOL;
		r->value.bool = bool;
	}
	g_hash_table_destroy(ht);
	
	return r;
}

static ESExpResult *
term_eval_not(ESExp *f, int argc, ESExpResult **argv, void *data)
{
	int res = TRUE;
	ESExpResult *r;

	if (argc>0) {
		if (argv[0]->type == ESEXP_RES_BOOL
		    && argv[0]->value.bool)
			res = FALSE;
	}
	r = e_sexp_result_new(f, ESEXP_RES_BOOL);
	r->value.bool = res;
	return r;
}

/* this should support all arguments ...? */
static ESExpResult *
term_eval_lt(ESExp *f, int argc, ESExpTerm **argv, void *data)
{
	ESExpResult *r, *r1, *r2;

	r = e_sexp_result_new(f, ESEXP_RES_UNDEFINED);
	
	if (argc == 2) {
		r1 = e_sexp_term_eval(f, argv[0]);
		r2 = e_sexp_term_eval(f, argv[1]);
		if (r1->type != r2->type) {
			e_sexp_result_free(f, r1);
			e_sexp_result_free(f, r2);
			e_sexp_result_free(f, r);
			e_sexp_fatal_error(f, _("Incompatible types in compare <"));
		} else if (r1->type == ESEXP_RES_INT) {
			r->type = ESEXP_RES_BOOL;
			r->value.bool = r1->value.number < r2->value.number;
		} else if (r1->type == ESEXP_RES_TIME) {
			r->type = ESEXP_RES_BOOL;
			r->value.bool = r1->value.time < r2->value.time;
		} else if (r1->type == ESEXP_RES_STRING) {
			r->type = ESEXP_RES_BOOL;
			r->value.bool = strcmp(r1->value.string, r2->value.string) < 0;
		}
		e_sexp_result_free(f, r1);
		e_sexp_result_free(f, r2);
	}
	return r;
}

/* this should support all arguments ...? */
static ESExpResult *
term_eval_gt(ESExp *f, int argc, ESExpTerm **argv, void *data)
{
	ESExpResult *r, *r1, *r2;

	r = e_sexp_result_new(f, ESEXP_RES_UNDEFINED);
	
	if (argc == 2) {
		r1 = e_sexp_term_eval(f, argv[0]);
		r2 = e_sexp_term_eval(f, argv[1]);
		if (r1->type != r2->type) {
			e_sexp_result_free(f, r1);
			e_sexp_result_free(f, r2);
			e_sexp_result_free(f, r);
			e_sexp_fatal_error(f, _("Incompatible types in compare >"));
		} else if (r1->type == ESEXP_RES_INT) {
			r->type = ESEXP_RES_BOOL;
			r->value.bool = r1->value.number > r2->value.number;
		} else if (r1->type == ESEXP_RES_TIME) {
			r->type = ESEXP_RES_BOOL;
			r->value.bool = r1->value.time > r2->value.time;
		} else if (r1->type == ESEXP_RES_STRING) {
			r->type = ESEXP_RES_BOOL;
			r->value.bool = strcmp(r1->value.string, r2->value.string) > 0;
		}
		e_sexp_result_free(f, r1);
		e_sexp_result_free(f, r2);
	}
	return r;
}

/* this should support all arguments ...? */
static ESExpResult *
term_eval_eq(ESExp *f, int argc, ESExpTerm **argv, void *data)
{
	ESExpResult *r, *r1, *r2;

	r = e_sexp_result_new(f, ESEXP_RES_BOOL);
	
	if (argc == 2) {
		r1 = e_sexp_term_eval(f, argv[0]);
		r2 = e_sexp_term_eval(f, argv[1]);
		if (r1->type != r2->type) {
			r->value.bool = FALSE;
		} else if (r1->type == ESEXP_RES_INT) {
			r->value.bool = r1->value.number == r2->value.number;
		} else if (r1->type == ESEXP_RES_BOOL) {
			r->value.bool = r1->value.bool == r2->value.bool;
		} else if (r1->type == ESEXP_RES_TIME) {
			r->value.bool = r1->value.time == r2->value.time;
		} else if (r1->type == ESEXP_RES_STRING) {
			r->value.bool = strcmp(r1->value.string, r2->value.string) == 0;
		}
		e_sexp_result_free(f, r1);
		e_sexp_result_free(f, r2);
	}
	return r;
}

static ESExpResult *
term_eval_plus(ESExp *f, int argc, ESExpResult **argv, void *data)
{
	ESExpResult *r=NULL;
	int type;
	int i;

	if (argc>0) {
		type = argv[0]->type;
		switch(type) {
		case ESEXP_RES_INT: {
			int total = argv[0]->value.number;
			for (i=1;i<argc && argv[i]->type == ESEXP_RES_INT;i++) {
				total += argv[i]->value.number;
			}
			if (i<argc) {
				e_sexp_resultv_free(f, argc, argv);
				e_sexp_fatal_error(f, _("Invalid types in (+ ints)"));
			}
			r = e_sexp_result_new(f, ESEXP_RES_INT);
			r->value.number = total;
			break; }
		case ESEXP_RES_STRING: {
			GString *s = g_string_new(argv[0]->value.string);
			for (i=1;i<argc && argv[i]->type == ESEXP_RES_STRING;i++) {
				g_string_append(s, argv[i]->value.string);
			}
			if (i<argc) {
				e_sexp_resultv_free(f, argc, argv);
				e_sexp_fatal_error(f, _("Invalid types in (+ strings)"));
			}
			r = e_sexp_result_new(f, ESEXP_RES_STRING);
			r->value.string = s->str;
			g_string_free(s, FALSE);
			break; }
		case ESEXP_RES_TIME: {
			time_t total;

			total = argv[0]->value.time;

			for (i = 1; i < argc && argv[i]->type == ESEXP_RES_TIME; i++)
				total += argv[i]->value.time;

			if (i < argc) {
				e_sexp_resultv_free (f, argc, argv);
				e_sexp_fatal_error (f, _("Invalid types in (+ time_t)"));
			}

			r = e_sexp_result_new (f, ESEXP_RES_TIME);
			r->value.time = total;
			break; }
		}
	}

	if (!r) {
		r = e_sexp_result_new(f, ESEXP_RES_INT);
		r->value.number = 0;
	}
	return r;
}

static ESExpResult *
term_eval_sub(ESExp *f, int argc, ESExpResult **argv, void *data)
{
	ESExpResult *r=NULL;
	int type;
	int i;

	if (argc>0) {
		type = argv[0]->type;
		switch(type) {
		case ESEXP_RES_INT: {
			int total = argv[0]->value.number;
			for (i=1;i<argc && argv[i]->type == ESEXP_RES_INT;i++) {
				total -= argv[i]->value.number;
			}
			if (i<argc) {
				e_sexp_resultv_free(f, argc, argv);
				e_sexp_fatal_error(f, _("Invalid types in -"));
			}
			r = e_sexp_result_new(f, ESEXP_RES_INT);
			r->value.number = total;
			break; }
		case ESEXP_RES_TIME: {
			time_t total;

			total = argv[0]->value.time;

			for (i = 1; i < argc && argv[i]->type == ESEXP_RES_TIME; i++)
				total -= argv[i]->value.time;

			if (i < argc) {
				e_sexp_resultv_free (f, argc, argv);
				e_sexp_fatal_error (f, _("Invalid types in (- time_t)"));
			}

			r = e_sexp_result_new (f, ESEXP_RES_TIME);
			r->value.time = total;
			break; }
		}
	}

	if (!r) {
		r = e_sexp_result_new(f, ESEXP_RES_INT);
		r->value.number = 0;
	}
	return r;
}

/* cast to int */
static ESExpResult *
term_eval_castint(ESExp *f, int argc, ESExpResult **argv, void *data)
{
	ESExpResult *r;

	if (argc != 1)
		e_sexp_fatal_error(f, _("Incorrect argument count to (int )"));

	r = e_sexp_result_new(f, ESEXP_RES_INT);
	switch (argv[0]->type) {
	case ESEXP_RES_INT:
		r->value.number = argv[0]->value.number;
		break;
	case ESEXP_RES_BOOL:
		r->value.number = argv[0]->value.bool != 0;
		break;
	case ESEXP_RES_STRING:
		r->value.number = strtoul(argv[0]->value.string, 0, 10);
		break;
	default:
		e_sexp_result_free(f, r);
		e_sexp_fatal_error(f, _("Invalid type in (cast-int )"));
	}

	return r;
}

/* cast to string */
static ESExpResult *
term_eval_caststring(ESExp *f, int argc, ESExpResult **argv, void *data)
{
	ESExpResult *r;

	if (argc != 1)
		e_sexp_fatal_error(f, _("Incorrect argument count to (cast-string )"));

	r = e_sexp_result_new(f, ESEXP_RES_STRING);
	switch (argv[0]->type) {
	case ESEXP_RES_INT:
		r->value.string = g_strdup_printf("%d", argv[0]->value.number);
		break;
	case ESEXP_RES_BOOL:
		r->value.string = g_strdup_printf("%d", argv[0]->value.bool != 0);
		break;
	case ESEXP_RES_STRING:
		r->value.string = g_strdup(argv[0]->value.string);
		break;
	default:
		e_sexp_result_free(f, r);
		e_sexp_fatal_error(f, _("Invalid type in (int )"));
	}

	return r;
}

/* implements 'if' function */
static ESExpResult *
term_eval_if(ESExp *f, int argc, ESExpTerm **argv, void *data)
{
	ESExpResult *r;
	int doit;

	if (argc >=2 && argc<=3) {
		r = e_sexp_term_eval(f, argv[0]);
		doit = (r->type == ESEXP_RES_BOOL && r->value.bool);
		e_sexp_result_free(f, r);
		if (doit) {
          if_satisified = TRUE;
			return e_sexp_term_eval(f, argv[1]);
		} else if (argc>2) {
			return e_sexp_term_eval(f, argv[2]);
		}
	}
	return e_sexp_result_new(f, ESEXP_RES_UNDEFINED);
}

/* implements 'begin' statement */
static ESExpResult *
term_eval_begin(ESExp *f, int argc, ESExpTerm **argv, void *data)
{
	ESExpResult *r=NULL;
	int i;

	for (i=0;i<argc;i++) {
		if (r)
			e_sexp_result_free(f, r);
		r = e_sexp_term_eval(f, argv[i]);
	}
	if (r)
		return r;
	else
		return e_sexp_result_new(f, ESEXP_RES_UNDEFINED);
}


/* this must only be called from inside term evaluation callbacks! */
ESExpResult *
e_sexp_term_eval(ESExp *f, ESExpTerm *t)
{
	ESExpResult *r = NULL;
	int i;
	ESExpResult **argv;

	g_return_val_if_fail(t != NULL, NULL);

	switch (t->type) {
	case ESEXP_TERM_STRING:
		r = e_sexp_result_new(f, ESEXP_RES_STRING);
		/* erk, this shoul;dn't need to strdup this ... */
		r->value.string = g_strdup(t->value.string);
		break;
	case ESEXP_TERM_INT:
		r = e_sexp_result_new(f, ESEXP_RES_INT);
		r->value.number = t->value.number;
		break;
	case ESEXP_TERM_BOOL:
		r = e_sexp_result_new(f, ESEXP_RES_BOOL);
		r->value.bool = t->value.bool;
		break;
	case ESEXP_TERM_TIME:
		r = e_sexp_result_new (f, ESEXP_RES_TIME);
		r->value.time = t->value.time;
		break;
	case ESEXP_TERM_IFUNC:
		if (t->value.func.sym->f.ifunc)
			r = t->value.func.sym->f.ifunc(f, t->value.func.termcount, t->value.func.terms, t->value.func.sym->data);
		break;
	case ESEXP_TERM_FUNC:
		/* first evaluate all arguments to result types */
		argv = alloca(sizeof(argv[0]) * t->value.func.termcount);
		for (i=0;i<t->value.func.termcount;i++) {
			argv[i] = e_sexp_term_eval(f, t->value.func.terms[i]);
		}
		/* call the function */
		if (t->value.func.sym->f.func)
			r = t->value.func.sym->f.func(f, t->value.func.termcount, argv, t->value.func.sym->data);

		e_sexp_resultv_free(f, t->value.func.termcount, argv);
		break;
        case ESEXP_TERM_VAR:
          //return e_sexp_term_eval (f, t->value.);
          break;
	default:
		e_sexp_fatal_error(f, _("Unknown type in parse tree: %d"), t->type);
	}

	if (r==NULL)
		r = e_sexp_result_new(f, ESEXP_RES_UNDEFINED);

	return r;
}

#ifdef TESTER
static void
eval_dump_result(ESExpResult *r, int depth)
{
	int i;
	
	if (r==NULL) {
		g_print("null result???\n");
		return;
	}

	for (i=0;i<depth;i++)
		g_print("   ");

	switch (r->type) {
	case ESEXP_RES_ARRAY_PTR:
		g_print("array pointers\n");
		break;
	case ESEXP_RES_INT:
		g_print("int: %d\n", r->value.number);
		break;
	case ESEXP_RES_STRING:
		g_print("string: '%s'\n", r->value.string);
		break;
	case ESEXP_RES_BOOL:
		g_print("bool: %c\n", r->value.bool?'t':'f');
		break;
	case ESEXP_RES_TIME:
		g_print("time_t: %ld\n", (long) r->value.time);
		break;
	case ESEXP_RES_UNDEFINED:
		g_print(" <undefined>\n");
		break;
	}
	g_print("\n");
}
#endif

void
parse_dump_term(ESExpTerm *t, int depth)
{
	int i;

	if (t==NULL) {
		g_print("null term??\n");
		return;
	}

	for (i=0;i<depth;i++)
		g_print("   ");
	
	switch (t->type) {
	case ESEXP_TERM_STRING:
		g_print(" \"%s\"", t->value.string);
		break;
	case ESEXP_TERM_INT:
		g_print(" %d", t->value.number);
		break;
	case ESEXP_TERM_BOOL:
		g_print(" #%c", t->value.bool?'t':'f');
		break;
	case ESEXP_TERM_TIME:
		g_print(" %ld", (long) t->value.time);
		break;
	case ESEXP_TERM_IFUNC:
	case ESEXP_TERM_FUNC:
		g_print(" (function %s\n", t->value.func.sym->name);
		/*g_print(" [%d] ", t->value.func.termcount);*/
		for (i=0;i<t->value.func.termcount;i++) {
			parse_dump_term(t->value.func.terms[i], depth+1);
		}
		for (i=0;i<depth;i++)
			g_print("   ");
		g_print(" )");
		break;
	case ESEXP_TERM_VAR:
		g_print(" (variable %s )\n", t->value.var->name);
		break;
	default:
		g_print("unknown type: %d\n", t->type);
	}

	g_print("\n");
}

/*
  PARSER
*/

static ESExpTerm *
parse_term_new(ESExp *f, int type)
{
	ESExpTerm *s = g_slice_new0 (ESExpTerm);
	s->type = type;
	return s;
}

static void
parse_term_free(ESExp *f, ESExpTerm *t)
{
	int i;

	if (t==NULL) {
		return;
	}
	
	switch (t->type) {
	case ESEXP_TERM_INT:
	case ESEXP_TERM_BOOL:
	case ESEXP_TERM_TIME:
	case ESEXP_TERM_VAR:
		break;

	case ESEXP_TERM_STRING:
		g_free(t->value.string);
		break;

	case ESEXP_TERM_FUNC:
	case ESEXP_TERM_IFUNC:
		for (i=0;i<t->value.func.termcount;i++) {
			parse_term_free(f, t->value.func.terms[i]);
		}
		g_free(t->value.func.terms);
		break;

	default:
		g_warning("parse_term_free: unknown type: %d", t->type);
	}
	g_slice_free(ESExpTerm, t);
}

static ESExpTerm **
parse_values(ESExp *f, int *len)
{
	int token;
	ESExpTerm **terms;
	int i, size = 0;
	GScanner *gs = f->scanner;
	GSList *list = NULL, *l;

	while ( (token = g_scanner_peek_next_token(gs)) != G_TOKEN_EOF
		&& token != ')') {
		list = g_slist_prepend(list, parse_value(f));
		size++;
	}

	/* go over the list, and put them backwards into the term array */
	terms = g_malloc(size * sizeof(*terms));
	l = list;
	for (i=size-1;i>=0;i--) {
		g_assert(l);
		g_assert(l->data);
		terms[i] = l->data;
		l = g_slist_next(l);
	}
	g_slist_free(list);

	*len = size;
	
	return terms;
}

static ESExpTerm *
parse_value(ESExp *f)
{
	int token, negative = FALSE;
	ESExpTerm *t = NULL;
	GScanner *gs = f->scanner;
	ESExpSymbol *s;
	
	token = g_scanner_get_next_token(gs);
	switch(token) {
	case G_TOKEN_LEFT_PAREN:
		return parse_list(f, TRUE);
	case G_TOKEN_STRING:
		t = parse_term_new(f, ESEXP_TERM_STRING);
		t->value.string = g_strdup(g_scanner_cur_value(gs).v_string);
		break;
	case '-':
		token = g_scanner_get_next_token (gs);
		if (token != G_TOKEN_INT) {
			e_sexp_fatal_error (f, _("Invalid format for a integer value"));
			return NULL;
		}
		
		negative = TRUE;
		/* fall through... */
	case G_TOKEN_INT:
		t = parse_term_new(f, ESEXP_TERM_INT);
		t->value.number = g_scanner_cur_value(gs).v_int;
		if (negative)
			t->value.number = -t->value.number;
		break;
	case '#': {
		char *str;
		token = g_scanner_get_next_token(gs);
		if (token != G_TOKEN_IDENTIFIER) {
			e_sexp_fatal_error (f, _("Invalid format for a boolean value"));
			return NULL;
		}
		
		str = g_scanner_cur_value (gs).v_identifier;
		
		g_assert (str != NULL);
		if (!(strlen (str) == 1 && (str[0] == 't' || str[0] == 'f'))) {
			e_sexp_fatal_error (f, _("Invalid format for a boolean value"));
			return NULL;
		}
		
		t = parse_term_new(f, ESEXP_TERM_BOOL);
		t->value.bool = (str[0] == 't');
		break; }
	case G_TOKEN_SYMBOL:
		s = g_scanner_cur_value(gs).v_symbol;
		switch (s->type) {
		case ESEXP_TERM_FUNC:
		case ESEXP_TERM_IFUNC:
				/* this is basically invalid, since we can't use function
				   pointers, but let the runtime catch it ... */
			t = parse_term_new(f, s->type);
			t->value.func.sym = s;
			t->value.func.terms = parse_values(f, &t->value.func.termcount);
			break;
		case ESEXP_TERM_VAR:
			t = parse_term_new(f, s->type);
			t->value.var = s;
			break;
		default:
			e_sexp_fatal_error(f, _("Invalid symbol type: %s: %d"), s->name, s->type);
		}
		break;
	case G_TOKEN_IDENTIFIER:
		e_sexp_fatal_error(f, _("Unknown identifier: %s"), g_scanner_cur_value(gs).v_identifier);
		break;
        case G_TOKEN_EOF:
          g_printerr("got eof\n");
          break;
	default:
		e_sexp_fatal_error(f, _("Unexpected token encountered: %d"), token);
	}
	return t;
}

/* FIXME: this needs some robustification */
static ESExpTerm *
parse_list(ESExp *f, int gotbrace)
{
	int token;
	ESExpTerm *t = NULL;
	GScanner *gs = f->scanner;

	if (gotbrace)
		token = '(';
	else
		token = g_scanner_get_next_token(gs);
	if (token =='(') {
		token = g_scanner_get_next_token(gs);
		switch(token) {
		case G_TOKEN_SYMBOL: {
			ESExpSymbol *s;

			s = g_scanner_cur_value(gs).v_symbol;
			t = parse_term_new(f, s->type);
			/* if we have a variable, find out its base type */
			while (s->type == ESEXP_TERM_VAR) {
				s = ((ESExpTerm *)(s->data))->value.var;
			}
			if (s->type == ESEXP_TERM_FUNC
			    || s->type == ESEXP_TERM_IFUNC) {
				t->value.func.sym = s;
				t->value.func.terms = parse_values(f, &t->value.func.termcount);
			} else {
				parse_term_free(f, t);
				e_sexp_fatal_error(f, _("Trying to call variable as function: %s"), s->name);
			}
			break; }
		case G_TOKEN_IDENTIFIER:
			e_sexp_fatal_error(f, _("Unknown identifier: %s"), g_scanner_cur_value(gs).v_identifier);
			break;
		default:
			e_sexp_fatal_error(f, _("Unexpected token encountered: %d"), token);
		}
		token = g_scanner_get_next_token(gs);
		if (token != ')') {
			e_sexp_fatal_error(f, _("Missing ')'"));
		}
	} else {
		e_sexp_fatal_error(f, _("Missing '('"));
	}

	return t;
}


/* 'builtin' functions */
static struct {
	char *name;
	ESExpFunc *func;
	int type;		/* set to 1 if a function can perform shortcut evaluation, or
				   doesn't execute everything, 0 otherwise */
} symbols[] = {
	{ "and", (ESExpFunc *)term_eval_and, 1 },
	{ "or", (ESExpFunc *)term_eval_or, 1 },
	{ "not", (ESExpFunc *)term_eval_not, 0 },
	{ "<", (ESExpFunc *)term_eval_lt, 1 },
	{ ">", (ESExpFunc *)term_eval_gt, 1 },
	{ "=", (ESExpFunc *)term_eval_eq, 1 },
	{ "+", (ESExpFunc *)term_eval_plus, 0 },
	{ "-", (ESExpFunc *)term_eval_sub, 0 },
	{ "cast-int", (ESExpFunc *)term_eval_castint, 0 },
	{ "cast-string", (ESExpFunc *)term_eval_caststring, 0 },
	{ "if", (ESExpFunc *)term_eval_if, 1 },
	{ "begin", (ESExpFunc *)term_eval_begin, 1 },
};

static void
free_symbol(void *key, void *value, void *data)
{
	ESExpSymbol *s = value;

	g_free(s->name);
	g_free(s);
}

static void
e_sexp_finalise(GObject *o)
{
	ESExp *s = (ESExp *)o;

	if (s->tree) {
		parse_term_free(s, s->tree);
		s->tree = NULL;
	}

	g_scanner_scope_foreach_symbol(s->scanner, 0, free_symbol, 0);
	g_scanner_destroy(s->scanner);

	G_OBJECT_CLASS (parent_class)->finalize (o);
}

static void
e_sexp_init (ESExp *s)
{
	int i;

	s->scanner = g_scanner_new(&scanner_config);

	/* load in builtin symbols? */
	for(i=0;i<sizeof(symbols)/sizeof(symbols[0]);i++) {
		if (symbols[i].type == 1) {
			e_sexp_add_ifunction(s, 0, symbols[i].name, (ESExpIFunc *)symbols[i].func, &symbols[i]);
		} else {
			e_sexp_add_function(s, 0, symbols[i].name, symbols[i].func, &symbols[i]);
		}
	}
}

static void
e_sexp_class_init (ESExpClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	parent_class = (GObjectClass*) g_type_class_peek_parent (klass);
	object_class->finalize = e_sexp_finalise;
}

G_DEFINE_TYPE(ESExp, e_sexp, G_TYPE_OBJECT)

ESExp *
e_sexp_new (void)
{
	ESExp *f = (ESExp *) g_object_new (E_TYPE_SEXP, NULL);
	return f;
}

void
e_sexp_add_function(ESExp *f, int scope, char *name, ESExpFunc *func, void *data)
{
	ESExpSymbol *s;

	g_return_if_fail (IS_E_SEXP (f));
	g_return_if_fail (name != NULL);

	e_sexp_remove_symbol (f, scope, name);

	s = g_malloc0(sizeof(*s));
	s->name = g_strdup(name);
	s->f.func = func;
	s->type = ESEXP_TERM_FUNC;
	s->data = data;
	g_scanner_scope_add_symbol(f->scanner, scope, s->name, s);
}

void
e_sexp_add_ifunction(ESExp *f, int scope, char *name, ESExpIFunc *ifunc, void *data)
{
	ESExpSymbol *s;

	g_return_if_fail (IS_E_SEXP (f));
	g_return_if_fail (name != NULL);

	e_sexp_remove_symbol (f, scope, name);

	s = g_malloc0(sizeof(*s));
	s->name = g_strdup(name);
	s->f.ifunc = ifunc;
	s->type = ESEXP_TERM_IFUNC;
	s->data = data;
	g_scanner_scope_add_symbol(f->scanner, scope, s->name, s);
}

void
e_sexp_add_variable(ESExp *f, int scope, char *name, ESExpTerm *value)
{
	ESExpSymbol *s;

	g_return_if_fail (IS_E_SEXP (f));
	g_return_if_fail (name != NULL);

	s = g_malloc0(sizeof(*s));
	s->name = g_strdup(name);
	s->type = ESEXP_TERM_VAR;
	s->data = value;
	g_scanner_scope_add_symbol(f->scanner, scope, s->name, s);
}

void
e_sexp_remove_symbol(ESExp *f, int scope, char *name)
{
	int oldscope;
	ESExpSymbol *s;

	g_return_if_fail (IS_E_SEXP (f));
	g_return_if_fail (name != NULL);

	oldscope = g_scanner_set_scope(f->scanner, scope);
	s = g_scanner_lookup_symbol(f->scanner, name);
	g_scanner_scope_remove_symbol(f->scanner, scope, name);
	g_scanner_set_scope(f->scanner, oldscope);
	if (s) {
		g_free(s->name);
		g_free(s);
	}
}

int
e_sexp_set_scope(ESExp *f, int scope)
{
	g_return_val_if_fail (IS_E_SEXP (f), 0);

	return g_scanner_set_scope(f->scanner, scope);
}

void
e_sexp_input_text(ESExp *f, const char *text, int len)
{
	g_return_if_fail (IS_E_SEXP (f));
	g_return_if_fail (text != NULL);

	g_scanner_input_text(f->scanner, text, len);
}

void
e_sexp_input_file (ESExp *f, int fd)
{
	g_return_if_fail (IS_E_SEXP (f));

	g_scanner_input_file(f->scanner, fd);
}

/* returns -1 on error */
int
e_sexp_parse(ESExp *f)
{
	g_return_val_if_fail (IS_E_SEXP (f), -1);

	if (setjmp(f->failenv)) {
		g_warning("Error in parsing: %s", f->error);
		return -1;
	}

	if (f->tree)
		parse_term_free(f, f->tree);

	f->tree = parse_value (f);

	return g_scanner_peek_next_token(f->scanner) != G_TOKEN_EOF;
}

/* returns NULL on error */
ESExpResult *
e_sexp_eval(ESExp *f)
{
	g_return_val_if_fail (IS_E_SEXP (f), NULL);
	g_return_val_if_fail (f->tree != NULL, NULL);

	if (setjmp(f->failenv)) {
		g_warning("Error in execution: %s", f->error);
		return NULL;
	}

	return e_sexp_term_eval(f, f->tree);
}

/**
 * e_sexp_encode_bool:
 * @s: 
 * @state: 
 * 
 * Encode a bool into an s-expression @s.  Bools are
 * encoded using #t #f syntax.
 **/
void
e_sexp_encode_bool(GString *s, gboolean state)
{
	if (state)
		g_string_append(s, " #t");
	else
		g_string_append(s, " #f");
}

/**
 * e_sexp_encode_string:
 * @s: Destination string.
 * @string: String expression.
 * 
 * Add a c string @string to the s-expression stored in
 * the gstring @s.  Quotes are added, and special characters
 * are escaped appropriately.
 **/
void
e_sexp_encode_string(GString *s, const char *string)
{
	char c;
	const char *p;

	if (string == NULL)
		p = "";
	else
		p = string;
	g_string_append(s, " \"");
	while ( (c = *p++) ) {
		if (c=='\\' || c=='\"' || c=='\'')
			g_string_append_c(s, '\\');
		g_string_append_c(s, c);
	}
	g_string_append(s, "\"");
}

#ifdef TESTER
int main(int argc, char **argv)
{
	ESExp *f;
	char *t = "(+ \"foo\" \"\\\"\" \"bar\" \"\\\\ blah \\x \")";
	ESExpResult *r;

	g_type_init ();

	f = e_sexp_new();

	e_sexp_add_variable(f, 0, "test", NULL);

	e_sexp_input_text(f, t, strlen(t));
	e_sexp_parse(f);

	if (f->tree) {
		parse_dump_term(f->tree, 0);
	}

	r = e_sexp_eval(f);
	if (r) {
		eval_dump_result(r, 0);
	} else {
		g_print("no result?|\n");
	}

	return 0;
}
#endif
