/*
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

/**
 * $Id$
 *
 * @brief Valuepair functions that are radiusd-specific and as such do not
 * 	  belong in the library.
 * @file src/lib/server/paircmp.c
 *
 * @ingroup AVP
 *
 * @copyright 2000,2006 The FreeRADIUS server project
 * @copyright 2000 Alan DeKok (aland@freeradius.org)
 */

RCSID("$Id$")

#include <freeradius-devel/server/paircmp.h>
#include <freeradius-devel/server/regex.h>
#include <freeradius-devel/server/request.h>
#include <freeradius-devel/unlang/xlat.h>
#include <freeradius-devel/util/debug.h>

#include <freeradius-devel/protocol/radius/rfc2865.h>
#include <freeradius-devel/protocol/freeradius/freeradius.internal.h>

#include <ctype.h>

typedef struct paircmp_s paircmp_t;
struct paircmp_s {
	fr_dict_attr_t const	*da;
	paircmp_t		*next;
};

static fr_dict_t const *dict_freeradius;
static fr_dict_t const *dict_radius;

extern fr_dict_autoload_t paircmp_dict[];
fr_dict_autoload_t paircmp_dict[] = {
	{ .out = &dict_freeradius, .proto = "freeradius" },
	{ .out = &dict_radius, .proto = "radius" },
	{ NULL }
};

static fr_dict_attr_t const *attr_crypt_password;
static fr_dict_attr_t const *attr_packet_dst_ip_address;
static fr_dict_attr_t const *attr_packet_dst_ipv6_address;
static fr_dict_attr_t const *attr_packet_dst_port;
static fr_dict_attr_t const *attr_packet_src_ip_address;
static fr_dict_attr_t const *attr_packet_src_ipv6_address;
static fr_dict_attr_t const *attr_packet_src_port;
static fr_dict_attr_t const *attr_request_processing_stage;
static fr_dict_attr_t const *attr_strip_user_name;
static fr_dict_attr_t const *attr_stripped_user_name;
static fr_dict_attr_t const *attr_user_name;
static fr_dict_attr_t const *attr_user_password;
static fr_dict_attr_t const *attr_virtual_server;

extern fr_dict_attr_autoload_t paircmp_dict_attr[];
fr_dict_attr_autoload_t paircmp_dict_attr[] = {
	{ .out = &attr_crypt_password, .name = "Password.Crypt", .type = FR_TYPE_STRING, .dict = &dict_freeradius },
	{ .out = &attr_packet_dst_ip_address, .name = "Packet-Dst-IP-Address", .type = FR_TYPE_IPV4_ADDR, .dict = &dict_freeradius },
	{ .out = &attr_packet_dst_ipv6_address, .name = "Packet-Dst-IPv6-Address", .type = FR_TYPE_IPV6_ADDR, .dict = &dict_freeradius },
	{ .out = &attr_packet_dst_port, .name = "Packet-Dst-Port", .type = FR_TYPE_UINT16, .dict = &dict_freeradius },
	{ .out = &attr_packet_src_ip_address, .name = "Packet-Src-IP-Address", .type = FR_TYPE_IPV4_ADDR, .dict = &dict_freeradius },
	{ .out = &attr_packet_src_ipv6_address, .name = "Packet-Src-IPv6-Address", .type = FR_TYPE_IPV6_ADDR, .dict = &dict_freeradius },
	{ .out = &attr_packet_src_port, .name = "Packet-Src-Port", .type = FR_TYPE_UINT16, .dict = &dict_freeradius },
	{ .out = &attr_request_processing_stage, .name = "Request-Processing-Stage", .type = FR_TYPE_STRING, .dict = &dict_freeradius },
	{ .out = &attr_strip_user_name, .name = "Strip-User-Name", .type = FR_TYPE_UINT32, .dict = &dict_freeradius },
	{ .out = &attr_stripped_user_name, .name = "Stripped-User-Name", .type = FR_TYPE_STRING, .dict = &dict_freeradius },
	{ .out = &attr_virtual_server, .name = "Virtual-Server", .type = FR_TYPE_STRING, .dict = &dict_freeradius },

	{ .out = &attr_user_name, .name = "User-Name", .type = FR_TYPE_STRING, .dict = &dict_radius },
	{ .out = &attr_user_password, .name = "User-Password", .type = FR_TYPE_STRING, .dict = &dict_radius },
	{ NULL }
};

static paircmp_t *cmp = NULL;

/*
 *	Generic comparisons, via xlat.
 */
static int generic_cmp(request_t *request, fr_pair_t const *check_item)
{
	PAIR_VERIFY(check_item);

	if ((check_item->op != T_OP_REG_EQ) && (check_item->op != T_OP_REG_NE)) {
		int rcode;
		char name[1024];
		char value[1024];
		fr_pair_t *vp;

		snprintf(name, sizeof(name), "%%{%s}", check_item->da->name);

		if (xlat_eval(value, sizeof(value), request, name, NULL, NULL) < 0) return 0;

		MEM(vp = fr_pair_afrom_da(request->request_ctx, check_item->da));
		vp->op = check_item->op;
		fr_pair_value_from_str(vp, value, strlen(value), &fr_value_unescape_single, false);

		/*
		 *	Paircmp returns 0 for failed comparison, 1 for succeeded -1 for error.
		 */
		rcode = fr_pair_cmp(check_item, vp);

		/*
		 *	We're being called from paircmp_func,
		 *	which wants 0 for success, and 1 for fail (sigh)
		 *
		 *	We should really fix the API so that it is
		 *	consistent.  i.e. the comparison callbacks should
		 *	return ONLY the resut of comparing A to B.
		 *	The radius_callback_cmp function should then
		 *	take care of using the operator to see if the
		 *	condition (A OP B) is true or not.
		 *
		 *	This would also allow "<", etc. to work in the
		 *	callback functions...
		 *
		 *	See rlm_ldap, ...groupcmp() for something that
		 *	returns 0 for matched, and 1 for didn't match.
		 */
		rcode = !rcode;
		talloc_free(vp);

		return rcode;
	}

	/*
	 *	Will do the xlat for us
	 */
	return paircmp_pairs(request, check_item, NULL);
}

/** Compares check and vp by value.
 *
 * Does not call any per-attribute comparison function, but does honour
 * check.operator. Basically does "vp.value check.op check.value".
 *
 * @param[in] request	Current request.
 * @param[in] check	rvalue, and operator.
 * @param[in] vp	lvalue.
 * @return
 *	- 0 if check and vp are equal
 *	- -1 if vp value is less than check value.
 *	- 1 is vp value is more than check value.
 *	- -2 on error.
 */
#ifdef HAVE_REGEX
int paircmp_pairs(request_t *request, fr_pair_t const *check, fr_pair_t *vp)
#else
int paircmp_pairs(UNUSED request_t *request, fr_pair_t const *check, fr_pair_t *vp)
#endif
{
	int rcode;

	/*
	 *      Check for =* and !* and return appropriately
	 */
	if (check->op == T_OP_CMP_TRUE)  return 0;
	if (check->op == T_OP_CMP_FALSE) return 1;

	if (!vp) {
		REDEBUG("Non-Unary operations require two operands");
		return -2;
	}

#ifdef HAVE_REGEX
	if ((check->op == T_OP_REG_EQ) || (check->op == T_OP_REG_NE)) {
		ssize_t		slen;
		regex_t		*preg = NULL;
		uint32_t	subcaptures;
		fr_regmatch_t	*regmatch;

		char *expr = NULL, *value = NULL;
		char const *expr_p, *value_p;

		if (check->vp_type == FR_TYPE_STRING) {
			expr_p = check->vp_strvalue;
		} else {
			fr_value_box_aprint(request, &expr, &check->data, NULL);
			expr_p = expr;
		}

		if (vp->vp_type == FR_TYPE_STRING) {
			value_p = vp->vp_strvalue;
		} else {
			fr_value_box_aprint(request, &value, &vp->data, NULL);
			value_p = value;
		}

		if (!expr_p || !value_p) {
			REDEBUG("Error stringifying operand for regular expression");

		regex_error:
			talloc_free(preg);
			talloc_free(expr);
			talloc_free(value);
			return -2;
		}

		/*
		 *	Include substring matches.
		 */
		slen = regex_compile(request, &preg, expr_p, talloc_array_length(expr_p) - 1,
				     NULL, true, true);
		if (slen <= 0) {
			REMARKER(expr_p, -slen, "%s", fr_strerror());

			goto regex_error;
		}

		subcaptures = regex_subcapture_count(preg);
		if (!subcaptures) subcaptures = REQUEST_MAX_REGEX + 1;	/* +1 for %{0} (whole match) capture group */
		MEM(regmatch = regex_match_data_alloc(NULL, subcaptures));

		/*
		 *	Evaluate the expression
		 */
		slen = regex_exec(preg, value_p, talloc_array_length(value_p) - 1, regmatch);
		if (slen < 0) {
			RPERROR("Invalid regex");

			goto regex_error;
		}

		if (check->op == T_OP_REG_EQ) {
			/*
			 *	Add in %{0}. %{1}, etc.
			 */
			regex_sub_to_request(request, &preg, &regmatch);
			rcode = (slen == 1) ? 0 : -1;
		} else {
			rcode = (slen != 1) ? 0 : -1;
		}

		talloc_free(regmatch);
		talloc_free(preg);
		talloc_free(expr);
		talloc_free(value);

		return rcode;
	}
#endif

	return fr_value_box_cmp_op(check->op, &vp->data, &check->data);
}

/** Compare check_item and request
 *
 * Unlike paircmp_pairs() this function will call any
 * attribute-specific comparison functions registered.  vp to be
 * matched is looked up from external sources depending on the
 * comparison function called.
 *
 * @param[in] request		Current request.
 * @param[in] da		the da to use
 * @param[in] op		operator to use
 * @param[in] value		value-box to use.
 * @return
 *	- 0 if check_item matches
 *	- -1 if check_item is smaller
 *	- 1 if check_item is larger
 */
int paircmp_virtual(request_t *request, fr_dict_attr_t const *da, fr_token_t op, fr_value_box_t const *value)
{
	paircmp_t *c;

	for (c = cmp; c; c = c->next) {
		if (c->da == da) {
			fr_pair_t *vp;
			int rcode;

			vp = fr_pair_afrom_da(request->request_ctx, da);
			if (!vp) return -1;

			vp->op = op;
			fr_value_box_copy(vp, &vp->data, value);

			rcode = generic_cmp(request, vp);
			talloc_free(vp);
			return rcode;
		}
	}

	return -1;
}


/** Find a comparison function for two attributes.
 *
 * @param[in] da	to find comparison function for.
 * @return
 *	- true if a comparison function was found.
 *	- false if a comparison function was not found.
 */
int paircmp_find(fr_dict_attr_t const *da)
{
	paircmp_t *c;

	for (c = cmp; c; c = c->next) if (c->da == da) return true;

	return false;
}

/** Unregister comparison function for an attribute
 *
 * @param[in] da		dict reference to unregister for.
 */
static void paircmp_unregister(fr_dict_attr_t const *da)
{
	paircmp_t *c, *last;

	last = NULL;
	for (c = cmp; c; c = c->next) {
		if (c->da == da) break;
		last = c;
	}

	if (c == NULL) return;

	if (last != NULL) {
		last->next = c->next;
	} else {
		cmp = c->next;
	}

	talloc_free(c);
}

/** Register a compare da.  We always use generic_cmp() for all comparisons.
 *
 * @param[in] da		to register comparison function for.
 * @return 0
 */
static int paircmp_register(fr_dict_attr_t const *da)
{
	paircmp_t *c;

	fr_assert(da != NULL);

	paircmp_unregister(da);

	MEM(c = talloc_zero(NULL, paircmp_t));
	c->da = da;
	c->next = cmp;
	cmp = c;

	return 0;
}

/** Add built in pair comparisons
 *
 */
int paircmp_init(void)
{
	if (fr_dict_autoload(paircmp_dict) < 0) {
		PERROR("%s", __FUNCTION__);
		return -1;
	}
	if (fr_dict_attr_autoload(paircmp_dict_attr) < 0) {
		PERROR("%s", __FUNCTION__);
		fr_dict_autofree(paircmp_dict);
		return -1;
	}

	paircmp_register(attr_packet_src_ip_address);
	paircmp_register(attr_packet_dst_ip_address);
	paircmp_register(attr_packet_src_port);
	paircmp_register(attr_packet_dst_port);
	paircmp_register(attr_packet_src_ipv6_address);
	paircmp_register(attr_packet_dst_ipv6_address);

	paircmp_register(attr_request_processing_stage);
	paircmp_register(attr_virtual_server);

	return 0;
}

void paircmp_free(void)
{
	paircmp_unregister(attr_packet_src_ip_address);
	paircmp_unregister(attr_packet_dst_ip_address);
	paircmp_unregister(attr_packet_src_port);
	paircmp_unregister(attr_packet_dst_port);
	paircmp_unregister(attr_packet_src_ipv6_address);
	paircmp_unregister(attr_packet_dst_ipv6_address);

	paircmp_unregister(attr_request_processing_stage);
	paircmp_unregister(attr_virtual_server);

	fr_dict_autofree(paircmp_dict);
}
