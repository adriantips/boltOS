/* ===========================================================================
 *  oldbrowser/ns_select.c -- real libcss selection handler bound to libdom.
 *
 *  A faithful port of NetSurf's content/handlers/css/select.c selection vtable.
 *  Every callback answers a question libcss asks about a DOM node, implemented
 *  with genuine libdom calls. The handful of NetSurf-isms are localised:
 *    - corestrings ("a", "href", node-data key) are interned here in init;
 *    - the default font family is fixed to sans-serif (no nsoption table);
 *    - :visited / :hover / presentational HTML hints are stubbed (no urldb /
 *      no hints.c yet) -- pure CSS selection is unaffected.
 * ===========================================================================*/
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include <libcss/libcss.h>
#include <dom/dom.h>

#include "ns_select.h"

/* ---- interned strings the handler needs (set up in ns_css_select_init) ---- */
static lwc_string *corestr_lwc_a;        /* "a" (anchor element name)          */
static lwc_string *corestr_lwc_universal;/* "*" (universal selector name)      */
static dom_string *corestr_dom_href;     /* "href"                             */
static dom_string *ns_node_data_key;     /* user-data key for libcss node data */
static int ns_inited;

/* css_select_style needs an empty media query + a unit context. */
css_unit_ctx ns_css_unit_ctx;
static css_media ns_css_media;

/* forward decl of the vtable so the node-data handler can reference it */
css_select_handler ns_select_handler;

/* =============================== callbacks ================================ */

static css_error node_name(void *pw, void *node, css_qname *qname)
{
	dom_node *n = node;
	dom_string *name;
	dom_exception err;

	(void) pw;
	err = dom_node_get_node_name(n, &name);
	if (err != DOM_NO_ERR)
		return CSS_NOMEM;

	qname->ns = NULL;

	err = dom_string_intern(name, &qname->name);
	if (err != DOM_NO_ERR) {
		dom_string_unref(name);
		return CSS_NOMEM;
	}

	dom_string_unref(name);
	return CSS_OK;
}

static css_error node_classes(void *pw, void *node,
		lwc_string ***classes, uint32_t *n_classes)
{
	dom_node *n = node;
	dom_exception err;

	(void) pw;
	*classes = NULL;
	*n_classes = 0;

	err = dom_element_get_classes(n, classes, n_classes);
	if (err != DOM_NO_ERR)
		return CSS_NOMEM;

	return CSS_OK;
}

static css_error node_id(void *pw, void *node, lwc_string **id)
{
	dom_node *n = node;
	dom_string *attr;
	dom_exception err;

	(void) pw;
	*id = NULL;

	err = dom_html_element_get_id(n, &attr);
	if (err != DOM_NO_ERR)
		return CSS_NOMEM;

	if (attr != NULL) {
		err = dom_string_intern(attr, id);
		if (err != DOM_NO_ERR) {
			dom_string_unref(attr);
			return CSS_NOMEM;
		}
		dom_string_unref(attr);
	}

	return CSS_OK;
}

static css_error named_ancestor_node(void *pw, void *node,
		const css_qname *qname, void **ancestor)
{
	(void) pw;
	dom_element_named_ancestor_node(node, qname->name,
			(struct dom_element **) ancestor);
	dom_node_unref(*ancestor);
	return CSS_OK;
}

static css_error named_parent_node(void *pw, void *node,
		const css_qname *qname, void **parent)
{
	(void) pw;
	dom_element_named_parent_node(node, qname->name,
			(struct dom_element **) parent);
	dom_node_unref(*parent);
	return CSS_OK;
}

static css_error named_sibling_node(void *pw, void *node,
		const css_qname *qname, void **sibling)
{
	dom_node *n = node;
	dom_node *prev;
	dom_exception err;

	(void) pw;
	*sibling = NULL;

	err = dom_node_get_previous_sibling(n, &n);
	if (err != DOM_NO_ERR)
		return CSS_OK;

	while (n != NULL) {
		dom_node_type type;

		err = dom_node_get_node_type(n, &type);
		if (err != DOM_NO_ERR) {
			dom_node_unref(n);
			return CSS_OK;
		}

		if (type == DOM_ELEMENT_NODE)
			break;

		err = dom_node_get_previous_sibling(n, &prev);
		if (err != DOM_NO_ERR) {
			dom_node_unref(n);
			return CSS_OK;
		}

		dom_node_unref(n);
		n = prev;
	}

	if (n != NULL) {
		dom_string *name;

		err = dom_node_get_node_name(n, &name);
		if (err != DOM_NO_ERR) {
			dom_node_unref(n);
			return CSS_OK;
		}

		dom_node_unref(n);

		if (dom_string_caseless_lwc_isequal(name, qname->name))
			*sibling = n;

		dom_string_unref(name);
	}

	return CSS_OK;
}

static css_error named_generic_sibling_node(void *pw, void *node,
		const css_qname *qname, void **sibling)
{
	dom_node *n = node;
	dom_node *prev;
	dom_exception err;

	(void) pw;
	*sibling = NULL;

	err = dom_node_get_previous_sibling(n, &n);
	if (err != DOM_NO_ERR)
		return CSS_OK;

	while (n != NULL) {
		dom_node_type type;
		dom_string *name;

		err = dom_node_get_node_type(n, &type);
		if (err != DOM_NO_ERR) {
			dom_node_unref(n);
			return CSS_OK;
		}

		if (type == DOM_ELEMENT_NODE) {
			err = dom_node_get_node_name(n, &name);
			if (err != DOM_NO_ERR) {
				dom_node_unref(n);
				return CSS_OK;
			}

			if (dom_string_caseless_lwc_isequal(name, qname->name)) {
				dom_string_unref(name);
				dom_node_unref(n);
				*sibling = n;
				break;
			}
			dom_string_unref(name);
		}

		err = dom_node_get_previous_sibling(n, &prev);
		if (err != DOM_NO_ERR) {
			dom_node_unref(n);
			return CSS_OK;
		}

		dom_node_unref(n);
		n = prev;
	}

	return CSS_OK;
}

static css_error parent_node(void *pw, void *node, void **parent)
{
	(void) pw;
	dom_element_parent_node(node, (struct dom_element **) parent);
	dom_node_unref(*parent);
	return CSS_OK;
}

static css_error sibling_node(void *pw, void *node, void **sibling)
{
	dom_node *n = node;
	dom_node *prev;
	dom_exception err;

	(void) pw;
	*sibling = NULL;

	err = dom_node_get_previous_sibling(n, &n);
	if (err != DOM_NO_ERR)
		return CSS_OK;

	while (n != NULL) {
		dom_node_type type;

		err = dom_node_get_node_type(n, &type);
		if (err != DOM_NO_ERR) {
			dom_node_unref(n);
			return CSS_OK;
		}

		if (type == DOM_ELEMENT_NODE)
			break;

		err = dom_node_get_previous_sibling(n, &prev);
		if (err != DOM_NO_ERR) {
			dom_node_unref(n);
			return CSS_OK;
		}

		dom_node_unref(n);
		n = prev;
	}

	if (n != NULL) {
		dom_node_unref(n);
		*sibling = n;
	}

	return CSS_OK;
}

static css_error node_has_name(void *pw, void *node,
		const css_qname *qname, bool *match)
{
	dom_node *n = node;

	(void) pw;
	if (lwc_string_isequal(qname->name, corestr_lwc_universal, match) ==
			lwc_error_ok && *match == false) {
		dom_string *name;
		dom_exception err;

		err = dom_node_get_node_name(n, &name);
		if (err != DOM_NO_ERR)
			return CSS_OK;

		/* element names are case insensitive in HTML */
		*match = dom_string_caseless_lwc_isequal(name, qname->name);

		dom_string_unref(name);
	}

	return CSS_OK;
}

static css_error node_has_class(void *pw, void *node,
		lwc_string *name, bool *match)
{
	dom_node *n = node;

	(void) pw;
	/* returns DOM_NO_ERR and *match unchanged for non-elements */
	dom_element_has_class(n, name, match);
	return CSS_OK;
}

static css_error node_has_id(void *pw, void *node,
		lwc_string *name, bool *match)
{
	dom_node *n = node;
	dom_string *attr;
	dom_exception err;

	(void) pw;
	*match = false;

	err = dom_html_element_get_id(n, &attr);
	if (err != DOM_NO_ERR)
		return CSS_OK;

	if (attr != NULL) {
		*match = dom_string_lwc_isequal(attr, name);
		dom_string_unref(attr);
	}

	return CSS_OK;
}

/* shared helper: look up an attribute value by qname; returns NULL if absent */
static dom_string *attr_value(dom_node *n, const css_qname *qname)
{
	dom_string *name, *val;
	dom_exception err;

	err = dom_string_create_interned(
			(const uint8_t *) lwc_string_data(qname->name),
			lwc_string_length(qname->name), &name);
	if (err != DOM_NO_ERR)
		return NULL;

	err = dom_element_get_attribute(n, name, &val);
	dom_string_unref(name);
	if (err != DOM_NO_ERR)
		return NULL;

	return val;
}

static css_error node_has_attribute(void *pw, void *node,
		const css_qname *qname, bool *match)
{
	dom_node *n = node;
	dom_string *name;
	dom_exception err;

	(void) pw;
	err = dom_string_create_interned(
			(const uint8_t *) lwc_string_data(qname->name),
			lwc_string_length(qname->name), &name);
	if (err != DOM_NO_ERR)
		return CSS_NOMEM;

	err = dom_element_has_attribute(n, name, match);
	dom_string_unref(name);
	if (err != DOM_NO_ERR)
		*match = false;

	return CSS_OK;
}

static css_error node_has_attribute_equal(void *pw, void *node,
		const css_qname *qname, lwc_string *value, bool *match)
{
	dom_node *n = node;
	dom_string *atr_val;

	(void) pw;
	if (lwc_string_length(value) == 0) { *match = false; return CSS_OK; }

	atr_val = attr_value(n, qname);
	if (atr_val == NULL) { *match = false; return CSS_OK; }

	*match = dom_string_caseless_lwc_isequal(atr_val, value);
	dom_string_unref(atr_val);
	return CSS_OK;
}

static css_error node_has_attribute_dashmatch(void *pw, void *node,
		const css_qname *qname, lwc_string *value, bool *match)
{
	dom_node *n = node;
	dom_string *atr_val;
	size_t vlen = lwc_string_length(value);

	(void) pw;
	if (vlen == 0) { *match = false; return CSS_OK; }

	atr_val = attr_value(n, qname);
	if (atr_val == NULL) { *match = false; return CSS_OK; }

	*match = dom_string_caseless_lwc_isequal(atr_val, value);
	if (*match == false) {
		const char *vdata = lwc_string_data(value);
		const char *data = (const char *) dom_string_data(atr_val);
		size_t len = dom_string_byte_length(atr_val);
		if (len > vlen && data[vlen] == '-' &&
				strncasecmp(data, vdata, vlen) == 0)
			*match = true;
	}

	dom_string_unref(atr_val);
	return CSS_OK;
}

static css_error node_has_attribute_includes(void *pw, void *node,
		const css_qname *qname, lwc_string *value, bool *match)
{
	dom_node *n = node;
	dom_string *atr_val;
	size_t vlen = lwc_string_length(value);
	const char *p, *start, *end;

	(void) pw;
	*match = false;
	if (vlen == 0) return CSS_OK;

	atr_val = attr_value(n, qname);
	if (atr_val == NULL) return CSS_OK;

	start = (const char *) dom_string_data(atr_val);
	end = start + dom_string_byte_length(atr_val);
	for (p = start; p <= end; p++) {
		if (*p == ' ' || *p == '\0') {
			if ((size_t) (p - start) == vlen &&
					strncasecmp(start, lwc_string_data(value),
						vlen) == 0) {
				*match = true;
				break;
			}
			start = p + 1;
		}
	}

	dom_string_unref(atr_val);
	return CSS_OK;
}

static css_error node_has_attribute_prefix(void *pw, void *node,
		const css_qname *qname, lwc_string *value, bool *match)
{
	dom_node *n = node;
	dom_string *atr_val;
	size_t vlen = lwc_string_length(value);

	(void) pw;
	if (vlen == 0) { *match = false; return CSS_OK; }

	atr_val = attr_value(n, qname);
	if (atr_val == NULL) { *match = false; return CSS_OK; }

	*match = dom_string_caseless_lwc_isequal(atr_val, value);
	if (*match == false) {
		const char *data = (const char *) dom_string_data(atr_val);
		size_t len = dom_string_byte_length(atr_val);
		if (len >= vlen &&
				strncasecmp(data, lwc_string_data(value), vlen) == 0)
			*match = true;
	}

	dom_string_unref(atr_val);
	return CSS_OK;
}

static css_error node_has_attribute_suffix(void *pw, void *node,
		const css_qname *qname, lwc_string *value, bool *match)
{
	dom_node *n = node;
	dom_string *atr_val;
	size_t vlen = lwc_string_length(value);

	(void) pw;
	if (vlen == 0) { *match = false; return CSS_OK; }

	atr_val = attr_value(n, qname);
	if (atr_val == NULL) { *match = false; return CSS_OK; }

	*match = dom_string_caseless_lwc_isequal(atr_val, value);
	if (*match == false) {
		const char *data = (const char *) dom_string_data(atr_val);
		size_t len = dom_string_byte_length(atr_val);
		const char *start = data + len - vlen;
		if (len >= vlen &&
				strncasecmp(start, lwc_string_data(value), vlen) == 0)
			*match = true;
	}

	dom_string_unref(atr_val);
	return CSS_OK;
}

static css_error node_has_attribute_substring(void *pw, void *node,
		const css_qname *qname, lwc_string *value, bool *match)
{
	dom_node *n = node;
	dom_string *atr_val;
	size_t vlen = lwc_string_length(value);

	(void) pw;
	if (vlen == 0) { *match = false; return CSS_OK; }

	atr_val = attr_value(n, qname);
	if (atr_val == NULL) { *match = false; return CSS_OK; }

	*match = dom_string_caseless_lwc_isequal(atr_val, value);
	if (*match == false) {
		const char *vdata = lwc_string_data(value);
		const char *start = (const char *) dom_string_data(atr_val);
		size_t len = dom_string_byte_length(atr_val);
		const char *last_start = start + len - vlen;
		if (len >= vlen) {
			while (start <= last_start) {
				if (strncasecmp(start, vdata, vlen) == 0) {
					*match = true;
					break;
				}
				start++;
			}
		}
	}

	dom_string_unref(atr_val);
	return CSS_OK;
}

static css_error node_is_root(void *pw, void *node, bool *match)
{
	dom_node *n = node;
	dom_node *parent;
	dom_node_type type;
	dom_exception err;

	(void) pw;
	err = dom_node_get_parent_node(n, &parent);
	if (err != DOM_NO_ERR)
		return CSS_NOMEM;

	if (parent != NULL) {
		err = dom_node_get_node_type(parent, &type);
		dom_node_unref(parent);
		if (err != DOM_NO_ERR)
			return CSS_NOMEM;
		if (type != DOM_DOCUMENT_NODE) {
			*match = false;
			return CSS_OK;
		}
	}

	*match = true;
	return CSS_OK;
}

static int node_count_siblings_check(dom_node *node, bool check_name,
		dom_string *name)
{
	dom_node_type type;
	int ret = 0;
	dom_exception exc;

	if (node == NULL)
		return 0;

	exc = dom_node_get_node_type(node, &type);
	if (exc != DOM_NO_ERR || type != DOM_ELEMENT_NODE)
		return 0;

	if (check_name) {
		dom_string *node_name = NULL;
		exc = dom_node_get_node_name(node, &node_name);
		if (exc == DOM_NO_ERR && node_name != NULL) {
			if (dom_string_caseless_isequal(name, node_name))
				ret = 1;
			dom_string_unref(node_name);
		}
	} else {
		ret = 1;
	}

	return ret;
}

static css_error node_count_siblings(void *pw, void *n, bool same_name,
		bool after, int32_t *count)
{
	int32_t cnt = 0;
	dom_exception exc;
	dom_string *node_name = NULL;

	(void) pw;
	if (same_name) {
		dom_node *node = n;
		exc = dom_node_get_node_name(node, &node_name);
		if (exc != DOM_NO_ERR || node_name == NULL)
			return CSS_NOMEM;
	}

	if (after) {
		dom_node *node = dom_node_ref(n);
		dom_node *next;
		do {
			exc = dom_node_get_next_sibling(node, &next);
			if (exc != DOM_NO_ERR)
				break;
			dom_node_unref(node);
			node = next;
			cnt += node_count_siblings_check(node, same_name, node_name);
		} while (node != NULL);
	} else {
		dom_node *node = dom_node_ref(n);
		dom_node *next;
		do {
			exc = dom_node_get_previous_sibling(node, &next);
			if (exc != DOM_NO_ERR)
				break;
			dom_node_unref(node);
			node = next;
			cnt += node_count_siblings_check(node, same_name, node_name);
		} while (node != NULL);
	}

	if (node_name != NULL)
		dom_string_unref(node_name);

	*count = cnt;
	return CSS_OK;
}

static css_error node_is_empty(void *pw, void *node, bool *match)
{
	dom_node *n = node, *next;
	dom_exception err;

	(void) pw;
	*match = true;

	err = dom_node_get_first_child(n, &n);
	if (err != DOM_NO_ERR)
		return CSS_BADPARM;

	while (n != NULL) {
		dom_node_type ntype;
		err = dom_node_get_node_type(n, &ntype);
		if (err != DOM_NO_ERR) {
			dom_node_unref(n);
			return CSS_BADPARM;
		}

		if (ntype == DOM_ELEMENT_NODE || ntype == DOM_TEXT_NODE) {
			*match = false;
			dom_node_unref(n);
			break;
		}

		err = dom_node_get_next_sibling(n, &next);
		if (err != DOM_NO_ERR) {
			dom_node_unref(n);
			return CSS_BADPARM;
		}
		dom_node_unref(n);
		n = next;
	}

	return CSS_OK;
}

static css_error node_is_link(void *pw, void *n, bool *match)
{
	dom_node *node = n;
	dom_exception exc;
	dom_string *node_name = NULL;

	(void) pw;
	exc = dom_node_get_node_name(node, &node_name);
	if (exc != DOM_NO_ERR || node_name == NULL)
		return CSS_NOMEM;

	if (dom_string_caseless_lwc_isequal(node_name, corestr_lwc_a)) {
		bool has_href;
		exc = dom_element_has_attribute(node, corestr_dom_href, &has_href);
		*match = (exc == DOM_NO_ERR && has_href);
	} else {
		*match = false;
	}

	dom_string_unref(node_name);
	return CSS_OK;
}

/* :visited needs a visited-URL database BoltOS does not wire here yet. */
static css_error node_is_visited(void *pw, void *node, bool *match)
{
	(void) pw; (void) node;
	*match = false;
	return CSS_OK;
}

static css_error node_is_hover(void *pw, void *node, bool *match)
{
	(void) pw; (void) node;
	*match = false;
	return CSS_OK;
}

static css_error node_is_active(void *pw, void *node, bool *match)
{
	(void) pw; (void) node;
	*match = false;
	return CSS_OK;
}

static css_error node_is_focus(void *pw, void *node, bool *match)
{
	(void) pw; (void) node;
	*match = false;
	return CSS_OK;
}

static css_error node_is_enabled(void *pw, void *node, bool *match)
{
	(void) pw; (void) node;
	*match = false;
	return CSS_OK;
}

static css_error node_is_disabled(void *pw, void *node, bool *match)
{
	(void) pw; (void) node;
	*match = false;
	return CSS_OK;
}

static css_error node_is_checked(void *pw, void *node, bool *match)
{
	(void) pw; (void) node;
	*match = false;
	return CSS_OK;
}

static css_error node_is_target(void *pw, void *node, bool *match)
{
	(void) pw; (void) node;
	*match = false;
	return CSS_OK;
}

static css_error node_is_lang(void *pw, void *node, lwc_string *lang,
		bool *match)
{
	(void) pw; (void) node; (void) lang;
	*match = false;
	return CSS_OK;
}

/* HTML presentational hints (bgcolor=, width=, align=, ...) come from hints.c,
 * not yet ported -- report none, so only real CSS applies. */
static css_error node_presentational_hint(void *pw, void *node,
		uint32_t *nhints, css_hint **hints)
{
	(void) pw; (void) node;
	*nhints = 0;
	*hints = NULL;
	return CSS_OK;
}

static css_error ua_default_for_property(void *pw, uint32_t property,
		css_hint *hint)
{
	(void) pw;
	if (property == CSS_PROP_COLOR) {
		hint->data.color = 0xff000000;          /* opaque black */
		hint->status = CSS_COLOR_COLOR;
	} else if (property == CSS_PROP_FONT_FAMILY) {
		hint->data.strings = NULL;
		hint->status = CSS_FONT_FAMILY_SANS_SERIF;
	} else if (property == CSS_PROP_QUOTES) {
		hint->data.strings = NULL;
		hint->status = CSS_QUOTES_NONE;
	} else if (property == CSS_PROP_VOICE_FAMILY) {
		hint->data.strings = NULL;
		hint->status = 0;
	} else {
		return CSS_INVALID;
	}
	return CSS_OK;
}

/* libdom user-data handler: keep libcss node data in step with DOM mutations. */
static void ns_dom_user_data_handler(dom_node_operation operation,
		dom_string *key, void *data, struct dom_node *src,
		struct dom_node *dst)
{
	if (dom_string_isequal(ns_node_data_key, key) == false || data == NULL)
		return;

	switch (operation) {
	case DOM_NODE_CLONED:
		css_libcss_node_data_handler(&ns_select_handler,
				CSS_NODE_CLONED, NULL, src, dst, data);
		break;
	case DOM_NODE_RENAMED:
		css_libcss_node_data_handler(&ns_select_handler,
				CSS_NODE_MODIFIED, NULL, src, NULL, data);
		break;
	case DOM_NODE_IMPORTED:
	case DOM_NODE_ADOPTED:
	case DOM_NODE_DELETED:
		css_libcss_node_data_handler(&ns_select_handler,
				CSS_NODE_DELETED, NULL, src, NULL, data);
		break;
	default:
		break;
	}
}

static css_error set_libcss_node_data(void *pw, void *node,
		void *libcss_node_data)
{
	dom_node *n = node;
	dom_exception err;
	void *old_node_data;

	(void) pw;
	err = dom_node_set_user_data(n, ns_node_data_key, libcss_node_data,
			ns_dom_user_data_handler, (void *) &old_node_data);
	if (err != DOM_NO_ERR)
		return CSS_NOMEM;

	return CSS_OK;
}

static css_error get_libcss_node_data(void *pw, void *node,
		void **libcss_node_data)
{
	dom_node *n = node;
	dom_exception err;

	(void) pw;
	err = dom_node_get_user_data(n, ns_node_data_key, libcss_node_data);
	if (err != DOM_NO_ERR)
		return CSS_NOMEM;

	return CSS_OK;
}

/* =============================== vtable =================================== */

css_select_handler ns_select_handler = {
	CSS_SELECT_HANDLER_VERSION_1,
	node_name,
	node_classes,
	node_id,
	named_ancestor_node,
	named_parent_node,
	named_sibling_node,
	named_generic_sibling_node,
	parent_node,
	sibling_node,
	node_has_name,
	node_has_class,
	node_has_id,
	node_has_attribute,
	node_has_attribute_equal,
	node_has_attribute_dashmatch,
	node_has_attribute_includes,
	node_has_attribute_prefix,
	node_has_attribute_suffix,
	node_has_attribute_substring,
	node_is_root,
	node_count_siblings,
	node_is_empty,
	node_is_link,
	node_is_visited,
	node_is_hover,
	node_is_active,
	node_is_focus,
	node_is_enabled,
	node_is_disabled,
	node_is_checked,
	node_is_target,
	node_is_lang,
	node_presentational_hint,
	ua_default_for_property,
	set_libcss_node_data,
	get_libcss_node_data,
};

/* ============================== public API =============================== */

int ns_css_select_init(void)
{
	if (ns_inited)
		return 0;

	if (lwc_intern_string("a", 1, &corestr_lwc_a) != lwc_error_ok)
		return -1;
	if (lwc_intern_string("*", 1, &corestr_lwc_universal) != lwc_error_ok)
		return -1;
	if (dom_string_create_interned((const uint8_t *) "href", 4,
			&corestr_dom_href) != DOM_NO_ERR)
		return -1;
	if (dom_string_create_interned(
			(const uint8_t *) "__ns_key_libcss_node_data",
			25, &ns_node_data_key) != DOM_NO_ERR)
		return -1;

	/* 16px default font, expressed as a CSS fixed-point value. */
	ns_css_unit_ctx.font_size_default = 16 * (1 << CSS_RADIX_POINT);
	ns_css_unit_ctx.font_size_minimum = 6 * (1 << CSS_RADIX_POINT);
	ns_css_unit_ctx.device_dpi = 96 * (1 << CSS_RADIX_POINT);
	ns_css_unit_ctx.root_style = NULL;

	/* an empty "screen" media descriptor: match everything */
	memset(&ns_css_media, 0, sizeof ns_css_media);
	ns_css_media.type = CSS_MEDIA_SCREEN;

	ns_inited = 1;
	return 0;
}

css_select_results *ns_css_select_style(css_select_ctx *select, dom_node *node,
		const css_computed_style *parent)
{
	css_select_results *sr = NULL;
	css_error err;

	err = css_select_style(select, node, &ns_css_unit_ctx, &ns_css_media,
			NULL, &ns_select_handler, NULL, &sr);
	if (err != CSS_OK || sr == NULL)
		return NULL;

	if (parent != NULL &&
			sr->styles[CSS_PSEUDO_ELEMENT_NONE] != NULL) {
		css_computed_style *composed = NULL;
		err = css_computed_style_compose(parent,
				sr->styles[CSS_PSEUDO_ELEMENT_NONE],
				&ns_css_unit_ctx, &composed);
		if (err == CSS_OK && composed != NULL) {
			css_computed_style_destroy(
					sr->styles[CSS_PSEUDO_ELEMENT_NONE]);
			sr->styles[CSS_PSEUDO_ELEMENT_NONE] = composed;
		}
	}

	return sr;
}
