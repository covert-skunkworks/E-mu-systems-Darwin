//  Description:
//
//		svg.c -- a small SVG rasterizer for the panel artwork (UI.svg).
//
//		Two passes over the text. The first collects the <clipPath>
//		definitions; the second walks the elements with a stack of group
//		states (transform, inherited style, active clips) and paints each
//		shape as it comes, so later shapes cover earlier ones.
//
//		A shape is flattened to polygons in device pixels (curves and arcs
//		become short lines), then rasterized with exact area coverage: each
//		edge adds its signed area to a per-row accumulation buffer, and a
//		running sum along the row gives the winding at each pixel, with the
//		fractional part at the edges as anti-aliasing:
//
//		  coverage(x, y) = min( 1, | sum[i <= x] acc(i, y) | )
//
//		Clamping |winding| to 1 is the non-zero rule. Strokes are built as
//		one positively wound polygon per segment, join and cap, so where
//		they overlap the sum only saturates: their union, without seams.
//
//		A clip path is rasterized the same way over the shape's area and
//		multiplied into its coverage. clipPathUnits="userSpaceOnUse" means
//		the clip is in the user space of the element that refers to it,
//		including that element's own transform.
//

//------------------------------------------------------------------------------
//------------------------------- Includes -------------------------------------
//------------------------------------------------------------------------------

#include "svg.h"

#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

//------------------------------------------------------------------------------
//--------------------- Local macros and constants -----------------------------
//------------------------------------------------------------------------------

#define SVG_DEPTH     64    // group nesting
#define SVG_MAX_CLIPS 16    // clips active at once
#define SVG_PI        3.14159265358979323846

enum { SVG_TAG_OPEN = 0, SVG_TAG_CLOSE, SVG_TAG_EMPTY };
enum { SVG_CAP_BUTT = 0, SVG_CAP_ROUND, SVG_CAP_SQUARE };
enum { SVG_JOIN_MITER = 0, SVG_JOIN_ROUND, SVG_JOIN_BEVEL };

//------------------------------------------------------------------------------
//---------------------- Local types and structures ----------------------------
//------------------------------------------------------------------------------

typedef struct
{
	double a, b, c, d, e, f;  // x' = a x + c y + e, y' = b x + d y + f
} svg_mat;

typedef struct
{
	uint32_t fill, stroke;  // 0xRRGGBB
	int      do_fill, do_stroke, hidden;
	float    fill_op, stroke_op, op;
	float    width, miter;
	int      cap, join;
} svg_style;

typedef struct
{
	float x, y;
} svg_pt;

typedef struct  // flattened outline in device pixels
{
	svg_pt  *p;
	int      n, cap;
	int     *sub;     // first point of each subpath
	uint8_t *closed;  // per subpath
	int      nsub, subcap, clcap;
} svg_path;

typedef struct
{
	float x0, y0, x1, y1;
} svg_edge;

typedef struct
{
	svg_edge *e;
	int       n, cap;
	float     minx, miny, maxx, maxy;
} svg_edges;

typedef struct
{
	const char *name;
	int         nlen;
	const char *attr;
	int         alen;
	int         kind;
} svg_tag;

typedef struct
{
	const char *id;
	int         idlen;
	const char *body, *end;  // the text inside <clipPath> ... </clipPath>
	svg_mat     m;           // the clipPath element's own transform
} svg_clipdef;

typedef struct
{
	const svg_clipdef *def;
	svg_mat            m;
} svg_clip;

typedef struct
{
	svg_mat   m;
	svg_style s;
	int       nclip;
} svg_state;

typedef struct
{
	uint32_t    *out;
	int          w, h;
	svg_clipdef *defs;
	int          ndefs, defcap;
	svg_clip     clips[SVG_MAX_CLIPS];
	int          nclips;
	float       *acc, *cov, *cov2;
	size_t       acccap, covcap;
	svg_path     path, cpath;
	svg_edges    ed, ced;
	int          oom;
} svg_ctx;

//------------------------------------------------------------------------------
//----------------------- Local function prototypes ----------------------------
//------------------------------------------------------------------------------

static int svg_grow( void **p, int *cap, int need, size_t size );
static int svg_next_tag( const char **pp, const char *end, svg_tag *t );
static int svg_is( const svg_tag *t, const char *name );
static int svg_attr( const svg_tag *t, const char *name, const char **v, int *vlen );
static const char *svg_num( const char *p, const char *e, double *v );
static svg_mat svg_mul( svg_mat a, svg_mat b );
static svg_pt svg_xf( const svg_mat *m, double x, double y );
static void svg_transform( const char *v, int vl, svg_mat *m );
static int svg_color( const char *v, int vl, uint32_t *c );
static void svg_prop( svg_style *s, const char *n, int nl, const char *v, int vl );
static void svg_style_of( const svg_tag *t, svg_style *s );
static void svg_move( svg_ctx *c, svg_path *pa, svg_pt p );
static void svg_line( svg_ctx *c, svg_path *pa, svg_pt p );
static void svg_cubic( svg_ctx *c, svg_path *pa, svg_pt p0, svg_pt p1, svg_pt p2, svg_pt p3 );
static void svg_arc( svg_ctx *c, svg_path *pa, const svg_mat *m, double x0, double y0, double rx, double ry,
					 double phi, int large, int sweep, double x1, double y1 );
static void svg_path_d( svg_ctx *c, svg_path *pa, const char *d, int dl, const svg_mat *m );
static void svg_points( svg_ctx *c, svg_path *pa, const char *v, int vl, const svg_mat *m, int close );
static void svg_ellipse( svg_ctx *c, svg_path *pa, const svg_mat *m, double cx, double cy, double rx, double ry );
static double svg_attr_num( const svg_tag *t, const char *name, double def );
static int svg_shape( svg_ctx *c, svg_path *pa, const svg_tag *t, const svg_mat *m );
static void svg_edge_add( svg_ctx *c, svg_edges *ed, svg_pt a, svg_pt b );
static void svg_poly( svg_ctx *c, svg_edges *ed, const svg_pt *p, int n );
static void svg_circle_poly( svg_ctx *c, svg_edges *ed, svg_pt o, float r );
static void svg_fill_edges( svg_ctx *c, const svg_path *pa, svg_edges *ed );
static void svg_stroke_edges( svg_ctx *c, svg_path *pa, svg_edges *ed, const svg_style *s, float hw );
static void svg_acc_line( float *acc, int w, int h, float x0, float y0, float x1, float y1 );
static void svg_acc_edge( float *acc, int w, int h, float x0, float y0, float x1, float y1 );
static void svg_cover( svg_ctx *c, const svg_edges *ed, int rx, int ry, int rw, int rh, float *cov );
static int svg_clip_edges( svg_ctx *c, const svg_clip *cl );
static void svg_paint( svg_ctx *c, const svg_edges *ed, uint32_t rgb, float alpha );
static void svg_draw( svg_ctx *c, const svg_tag *t, const svg_state *st );
static const svg_clipdef *svg_find_clip( svg_ctx *c, const char *v, int vl );
static int svg_collect_clips( svg_ctx *c, const char *doc, const char *end );

//------------------------------------------------------------------------------
//---------------------------- Local functions ---------------------------------
//------------------------------------------------------------------------------

/**
  * @brief  Make room for need elements in a growable array.
  *
  * @param  p The array (may be reallocated).
  * @param  cap Its capacity in elements.
  * @param  need Elements needed.
  * @param  size Element size.
  *
  * @retval 0 room, -1 out of memory.
  */
static int svg_grow( void **p, int *cap, int need, size_t size )
{
	int n = *cap ? *cap : 64;
	void *q;

	if ( need <= *cap )
	{
		return 0;
	}
	while ( n < need )
	{
		n *= 2;
	}
	q = realloc( *p, (size_t)n * size );
	if ( !q )
	{
		return -1;
	}
	*p = q;
	*cap = n;
	return 0;
}

/**
  * @brief  Find the next element tag, skipping text, comments and declarations.
  *
  * @param  pp Scan position (advanced past the tag).
  * @param  end End of the text.
  * @param  t Receives the tag.
  *
  * @retval 1 a tag, 0 end of text.
  */
static int svg_next_tag( const char **pp, const char *end, svg_tag *t )
{
	const char *p = *pp;
	char quote = 0;

	for ( ;; )
	{
		while ( p < end && *p != '<' )
		{
			p++;
		}
		if ( p + 1 >= end )
		{
			*pp = end;
			return 0;
		}
		if ( p + 4 <= end && !memcmp( p, "<!--", 4 ) )
		{
			p += 4;
			while ( p + 3 <= end && memcmp( p, "-->", 3 ) )
			{
				p++;
			}
			p += 3;
			continue;
		}
		if ( p[1] == '?' || p[1] == '!' )
		{
			while ( p < end && *p != '>' )
			{
				p++;
			}
			continue;
		}
		break;
	}
	p++;
	t->kind = SVG_TAG_OPEN;
	if ( *p == '/' )
	{
		t->kind = SVG_TAG_CLOSE;
		p++;
	}
	t->name = p;
	while ( p < end && !isspace( (unsigned char)*p ) && *p != '>' && *p != '/' )
	{
		p++;
	}
	t->nlen = (int)( p - t->name );
	t->attr = p;
	while ( p < end && ( quote || *p != '>' ) )
	{
		if ( quote )
		{
			quote = *p == quote ? 0 : quote;
		}
		else if ( *p == '"' || *p == '\'' )
		{
			quote = *p;
		}
		p++;
	}
	t->alen = (int)( p - t->attr );
	if ( t->alen > 0 && p[-1] == '/' )
	{
		t->kind = SVG_TAG_EMPTY;
		t->alen--;
	}
	*pp = p < end ? p + 1 : end;
	return 1;
}

/**
  * @brief  Is the tag this element?
  *
  * @param  t The tag.
  * @param  name Element name (with any namespace prefix).
  *
  * @retval 1 yes, 0 no.
  */
static int svg_is( const svg_tag *t, const char *name )
{
	return (int)strlen( name ) == t->nlen && !memcmp( t->name, name, (size_t)t->nlen );
}

/**
  * @brief  Find an attribute's value.
  *
  * @param  t The tag.
  * @param  name Attribute name.
  * @param  v Receives the value (not terminated).
  * @param  vlen Receives its length.
  *
  * @retval 1 found, 0 not there.
  */
static int svg_attr( const svg_tag *t, const char *name, const char **v, int *vlen )
{
	const char *p = t->attr, *e = t->attr + t->alen, *n, *vs;
	int nl = (int)strlen( name ), len;
	char quote;

	while ( p < e )
	{
		while ( p < e && isspace( (unsigned char)*p ) )
		{
			p++;
		}
		n = p;
		while ( p < e && *p != '=' && !isspace( (unsigned char)*p ) )
		{
			p++;
		}
		len = (int)( p - n );
		while ( p < e && isspace( (unsigned char)*p ) )
		{
			p++;
		}
		if ( p >= e || *p != '=' )
		{
			continue;
		}
		p++;
		while ( p < e && isspace( (unsigned char)*p ) )
		{
			p++;
		}
		if ( p >= e || ( *p != '"' && *p != '\'' ) )
		{
			return 0;
		}
		quote = *p++;
		vs = p;
		while ( p < e && *p != quote )
		{
			p++;
		}
		if ( len == nl && !memcmp( n, name, (size_t)nl ) )
		{
			*v = vs;
			*vlen = (int)( p - vs );
			return 1;
		}
		p++;
	}
	return 0;
}

/**
  * @brief  Read a number, skipping white space and commas before it.
  *
  * @param  p Position.
  * @param  e End.
  * @param  v Receives the number.
  *
  * @retval Position after the number, NULL if there is none.
  */
static const char *svg_num( const char *p, const char *e, double *v )
{
	char buf[64];
	int n = 0, digits = 0;

	while ( p < e && ( isspace( (unsigned char)*p ) || *p == ',' ) )
	{
		p++;
	}
	if ( p < e && ( *p == '+' || *p == '-' ) )
	{
		buf[n++] = *p++;
	}
	while ( p < e && isdigit( (unsigned char)*p ) && n < 60 )
	{
		buf[n++] = *p++;
		digits++;
	}
	if ( p < e && *p == '.' && n < 60 )
	{
		buf[n++] = *p++;
		while ( p < e && isdigit( (unsigned char)*p ) && n < 60 )
		{
			buf[n++] = *p++;
			digits++;
		}
	}
	if ( !digits )
	{
		return NULL;
	}
	if ( p + 1 < e && ( *p == 'e' || *p == 'E' ) &&
		 ( isdigit( (unsigned char)p[1] ) || ( ( p[1] == '-' || p[1] == '+' ) && p + 2 < e && isdigit( (unsigned char)p[2] ) ) ) )
	{
		buf[n++] = *p++;
		buf[n++] = *p++;
		while ( p < e && isdigit( (unsigned char)*p ) && n < 63 )
		{
			buf[n++] = *p++;
		}
	}
	buf[n] = 0;
	*v = strtod( buf, NULL );
	return p;
}

/**
  * @brief  Compose two transforms: the result applies b first, then a.
  *
  * @param  a Outer transform.
  * @param  b Inner transform.
  *
  * @retval a * b.
  */
static svg_mat svg_mul( svg_mat a, svg_mat b )
{
	svg_mat r;

	r.a = a.a * b.a + a.c * b.b;
	r.b = a.b * b.a + a.d * b.b;
	r.c = a.a * b.c + a.c * b.d;
	r.d = a.b * b.c + a.d * b.d;
	r.e = a.a * b.e + a.c * b.f + a.e;
	r.f = a.b * b.e + a.d * b.f + a.f;
	return r;
}

/**
  * @brief  Transform a point to device pixels.
  *
  * @param  m The transform.
  * @param  x User x.
  * @param  y User y.
  *
  * @retval The device point.
  */
static svg_pt svg_xf( const svg_mat *m, double x, double y )
{
	svg_pt p;

	p.x = (float)( m->a * x + m->c * y + m->e );
	p.y = (float)( m->b * x + m->d * y + m->f );
	return p;
}

/**
  * @brief  Apply a transform attribute: m = m * (each transform in turn).
  *
  * @param  v The attribute value.
  * @param  vl Its length.
  * @param  m The transform, updated.
  *
  * @retval None.
  */
static void svg_transform( const char *v, int vl, svg_mat *m )
{
	const char *p = v, *e = v + vl, *q;
	double a[6];
	svg_mat t;
	int n, kind;

	while ( p < e )
	{
		while ( p < e && ( isspace( (unsigned char)*p ) || *p == ',' ) )
		{
			p++;
		}
		if ( p >= e )
		{
			break;
		}
		kind = !strncmp( p, "matrix", 6 ) ? 0 : !strncmp( p, "translate", 9 ) ? 1 : !strncmp( p, "scale", 5 ) ? 2 :
			   !strncmp( p, "rotate", 6 ) ? 3 : -1;
		while ( p < e && *p != '(' )
		{
			p++;
		}
		p++;
		for ( n = 0; n < 6 && ( q = svg_num( p, e, &a[n] ) ); n++ )
		{
			p = q;
		}
		while ( p < e && *p != ')' )
		{
			p++;
		}
		p++;
		t = ( svg_mat ){ 1, 0, 0, 1, 0, 0 };
		if ( kind == 0 && n == 6 )
		{
			t = ( svg_mat ){ a[0], a[1], a[2], a[3], a[4], a[5] };
		}
		else if ( kind == 1 && n >= 1 )
		{
			t.e = a[0];
			t.f = n > 1 ? a[1] : 0;
		}
		else if ( kind == 2 && n >= 1 )
		{
			t.a = a[0];
			t.d = n > 1 ? a[1] : a[0];
		}
		else if ( kind == 3 && n >= 1 )
		{
			double r = a[0] * SVG_PI / 180, cx = n >= 3 ? a[1] : 0, cy = n >= 3 ? a[2] : 0;

			t = ( svg_mat ){ cos( r ), sin( r ), -sin( r ), cos( r ), 0, 0 };
			t.e = cx - t.a * cx - t.c * cy;
			t.f = cy - t.b * cx - t.d * cy;
		}
		*m = svg_mul( *m, t );
	}
}

/**
  * @brief  Parse a paint value.
  *
  * @param  v The value.
  * @param  vl Its length.
  * @param  c Receives 0xRRGGBB.
  *
  * @retval 1 a colour, 0 none, -1 not understood (gradients: left as inherited).
  */
static int svg_color( const char *v, int vl, uint32_t *c )
{
	static const struct
	{
		const char *name;
		uint32_t    rgb;
	} named[] = {
		{ "black", 0x000000 }, { "white", 0xFFFFFF }, { "red", 0xFF0000 }, { "lime", 0x00FF00 },
		{ "green", 0x008000 }, { "blue", 0x0000FF }, { "yellow", 0xFFFF00 }, { "gray", 0x808080 },
		{ "grey", 0x808080 }, { "silver", 0xC0C0C0 }, { "orange", 0xFFA500 }, { "currentColor", 0x000000 },
	};
	char hex[8];
	double r, g, b;
	const char *p, *e = v + vl;
	int i;

	while ( vl && isspace( (unsigned char)*v ) )
	{
		v++;
		vl--;
	}
	while ( vl && isspace( (unsigned char)v[vl - 1] ) )
	{
		vl--;
	}
	if ( vl == 4 && !memcmp( v, "none", 4 ) )
	{
		return 0;
	}
	if ( vl == 7 && v[0] == '#' )
	{
		memcpy( hex, v + 1, 6 );
		hex[6] = 0;
		*c = (uint32_t)strtoul( hex, NULL, 16 );
		return 1;
	}
	if ( vl == 4 && v[0] == '#' )
	{
		for ( *c = 0, i = 1; i < 4; i++ )
		{
			hex[0] = v[i];
			hex[1] = 0;
			*c = ( *c << 8 ) | ( (uint32_t)strtoul( hex, NULL, 16 ) * 0x11 );
		}
		return 1;
	}
	if ( vl > 4 && !memcmp( v, "rgb(", 4 ) && ( p = svg_num( v + 4, e, &r ) ) && ( p = svg_num( p, e, &g ) ) &&
		 svg_num( p, e, &b ) )
	{
		*c = ( (uint32_t)r & 0xFF ) << 16 | ( (uint32_t)g & 0xFF ) << 8 | ( (uint32_t)b & 0xFF );
		return 1;
	}
	for ( i = 0; i < (int)( sizeof named / sizeof named[0] ); i++ )
	{
		if ( (int)strlen( named[i].name ) == vl && !memcmp( v, named[i].name, (size_t)vl ) )
		{
			*c = named[i].rgb;
			return 1;
		}
	}
	return -1;
}

/**
  * @brief  Apply one presentation property.
  *
  * @param  s The style, updated.
  * @param  n Property name.
  * @param  nl Its length.
  * @param  v Value.
  * @param  vl Its length.
  *
  * @retval None.
  */
static void svg_prop( svg_style *s, const char *n, int nl, const char *v, int vl )
{
	char buf[32];
	double x;
	int k;

	#define SVG_IS( name ) ( nl == (int)sizeof( name ) - 1 && !memcmp( n, name, sizeof( name ) - 1 ) )
	#define SVG_VAL( name ) ( vl == (int)sizeof( name ) - 1 && !memcmp( v, name, sizeof( name ) - 1 ) )
	k = vl < 31 ? vl : 31;
	memcpy( buf, v, (size_t)k );
	buf[k] = 0;
	x = strtod( buf, NULL );
	if ( SVG_IS( "fill" ) )
	{
		k = svg_color( v, vl, &s->fill );
		s->do_fill = k < 0 ? s->do_fill : k;
	}
	else if ( SVG_IS( "stroke" ) )
	{
		k = svg_color( v, vl, &s->stroke );
		s->do_stroke = k < 0 ? s->do_stroke : k;
	}
	else if ( SVG_IS( "fill-opacity" ) )
	{
		s->fill_op = (float)x;
	}
	else if ( SVG_IS( "stroke-opacity" ) )
	{
		s->stroke_op = (float)x;
	}
	else if ( SVG_IS( "opacity" ) )  // group opacity approximated by multiplying it into the children
	{
		s->op *= (float)x;
	}
	else if ( SVG_IS( "stroke-width" ) )
	{
		s->width = (float)x;
	}
	else if ( SVG_IS( "stroke-miterlimit" ) )
	{
		s->miter = (float)x;
	}
	else if ( SVG_IS( "stroke-linecap" ) )
	{
		s->cap = SVG_VAL( "round" ) ? SVG_CAP_ROUND : SVG_VAL( "square" ) ? SVG_CAP_SQUARE : SVG_CAP_BUTT;
	}
	else if ( SVG_IS( "stroke-linejoin" ) )
	{
		s->join = SVG_VAL( "round" ) ? SVG_JOIN_ROUND : SVG_VAL( "bevel" ) ? SVG_JOIN_BEVEL : SVG_JOIN_MITER;
	}
	else if ( SVG_IS( "display" ) && SVG_VAL( "none" ) )
	{
		s->hidden = 1;
	}
	else if ( SVG_IS( "visibility" ) )
	{
		s->hidden = SVG_VAL( "hidden" ) || SVG_VAL( "collapse" );
	}
	#undef SVG_IS
	#undef SVG_VAL
}

/**
  * @brief  Apply an element's presentation attributes, then its style attribute.
  *
  * @param  t The element.
  * @param  s The inherited style, updated.
  *
  * @retval None.
  */
static void svg_style_of( const svg_tag *t, svg_style *s )
{
	static const char *const props[] = { "fill", "stroke", "fill-opacity", "stroke-opacity", "opacity",
		"stroke-width", "stroke-miterlimit", "stroke-linecap", "stroke-linejoin", "display", "visibility" };
	const char *v, *e, *n, *nv;
	int vl, i, nl, nvl;

	for ( i = 0; i < (int)( sizeof props / sizeof props[0] ); i++ )
	{
		if ( svg_attr( t, props[i], &v, &vl ) )
		{
			svg_prop( s, props[i], (int)strlen( props[i] ), v, vl );
		}
	}
	if ( !svg_attr( t, "style", &v, &vl ) )
	{
		return;
	}
	e = v + vl;
	while ( v < e )
	{
		while ( v < e && ( isspace( (unsigned char)*v ) || *v == ';' ) )
		{
			v++;
		}
		n = v;
		while ( v < e && *v != ':' && *v != ';' )
		{
			v++;
		}
		nl = (int)( v - n );
		while ( nl && isspace( (unsigned char)n[nl - 1] ) )
		{
			nl--;
		}
		if ( v >= e || *v != ':' )
		{
			continue;
		}
		v++;
		while ( v < e && isspace( (unsigned char)*v ) )
		{
			v++;
		}
		nv = v;
		while ( v < e && *v != ';' )
		{
			v++;
		}
		nvl = (int)( v - nv );
		while ( nvl && isspace( (unsigned char)nv[nvl - 1] ) )
		{
			nvl--;
		}
		svg_prop( s, n, nl, nv, nvl );
	}
}

/**
  * @brief  Start a subpath.
  *
  * @param  c Context (out-of-memory flag).
  * @param  pa The outline.
  * @param  p First point (device).
  *
  * @retval None.
  */
static void svg_move( svg_ctx *c, svg_path *pa, svg_pt p )
{
	if ( svg_grow( (void **)&pa->sub, &pa->subcap, pa->nsub + 1, sizeof *pa->sub ) ||
		 svg_grow( (void **)&pa->closed, &pa->clcap, pa->nsub + 1, 1 ) ||
		 svg_grow( (void **)&pa->p, &pa->cap, pa->n + 1, sizeof *pa->p ) )
	{
		c->oom = 1;
		return;
	}
	pa->sub[pa->nsub] = pa->n;
	pa->closed[pa->nsub++] = 0;
	pa->p[pa->n++] = p;
}

/**
  * @brief  Add a point to the current subpath.
  *
  * @param  c Context.
  * @param  pa The outline.
  * @param  p The point (device).
  *
  * @retval None.
  */
static void svg_line( svg_ctx *c, svg_path *pa, svg_pt p )
{
	if ( !pa->nsub )
	{
		svg_move( c, pa, p );
		return;
	}
	if ( svg_grow( (void **)&pa->p, &pa->cap, pa->n + 1, sizeof *pa->p ) )
	{
		c->oom = 1;
		return;
	}
	pa->p[pa->n++] = p;
}

/**
  * @brief  Flatten a cubic Bezier (device points) into lines.
  *
  * @param  c Context.
  * @param  pa The outline.
  * @param  p0 Start (already in the outline).
  * @param  p1 Control 1.
  * @param  p2 Control 2.
  * @param  p3 End.
  *
  * @retval None.
  */
static void svg_cubic( svg_ctx *c, svg_path *pa, svg_pt p0, svg_pt p1, svg_pt p2, svg_pt p3 )
{
	float len = hypotf( p1.x - p0.x, p1.y - p0.y ) + hypotf( p2.x - p1.x, p2.y - p1.y ) + hypotf( p3.x - p2.x, p3.y - p2.y );
	float t, u;
	int n = 1 + (int)( sqrtf( len ) * 1.5f ), i;
	svg_pt q;

	n = n > 128 ? 128 : n;
	for ( i = 1; i <= n; i++ )
	{
		t = (float)i / n;
		u = 1 - t;
		q.x = u * u * u * p0.x + 3 * u * u * t * p1.x + 3 * u * t * t * p2.x + t * t * t * p3.x;
		q.y = u * u * u * p0.y + 3 * u * u * t * p1.y + 3 * u * t * t * p2.y + t * t * t * p3.y;
		svg_line( c, pa, q );
	}
}

/**
  * @brief  An elliptical arc (SVG endpoint form, user units) as cubics.
  *
  * @param  c Context.
  * @param  pa The outline.
  * @param  m User to device.
  * @param  x0 Start x.
  * @param  y0 Start y.
  * @param  rx Radius x.
  * @param  ry Radius y.
  * @param  phi X-axis rotation, degrees.
  * @param  large Large-arc flag.
  * @param  sweep Sweep flag.
  * @param  x1 End x.
  * @param  y1 End y.
  *
  * @retval None.
  */
static void svg_arc( svg_ctx *c, svg_path *pa, const svg_mat *m, double x0, double y0, double rx, double ry,
					 double phi, int large, int sweep, double x1, double y1 )
{
	double cp, sp, dx, dy, x1p, y1p, lam, num, den, co, cxp, cyp, cx, cy, th, dth, ux, uy, vx, vy, k, a0, a1;
	double e0x, e0y, e1x, e1y, d0x, d0y, d1x, d1y;
	int n, i;

	rx = fabs( rx );
	ry = fabs( ry );
	if ( rx == 0 || ry == 0 || ( x0 == x1 && y0 == y1 ) )
	{
		svg_line( c, pa, svg_xf( m, x1, y1 ) );
		return;
	}
	phi *= SVG_PI / 180;
	cp = cos( phi );
	sp = sin( phi );
	dx = ( x0 - x1 ) / 2;
	dy = ( y0 - y1 ) / 2;
	x1p = cp * dx + sp * dy;
	y1p = -sp * dx + cp * dy;
	lam = ( x1p * x1p ) / ( rx * rx ) + ( y1p * y1p ) / ( ry * ry );
	if ( lam > 1 )
	{
		rx *= sqrt( lam );
		ry *= sqrt( lam );
	}
	num = rx * rx * ry * ry - rx * rx * y1p * y1p - ry * ry * x1p * x1p;
	den = rx * rx * y1p * y1p + ry * ry * x1p * x1p;
	co = den > 0 && num > 0 ? sqrt( num / den ) : 0;
	co = large == sweep ? -co : co;
	cxp = co * rx * y1p / ry;
	cyp = -co * ry * x1p / rx;
	cx = cp * cxp - sp * cyp + ( x0 + x1 ) / 2;
	cy = sp * cxp + cp * cyp + ( y0 + y1 ) / 2;
	ux = ( x1p - cxp ) / rx;
	uy = ( y1p - cyp ) / ry;
	vx = ( -x1p - cxp ) / rx;
	vy = ( -y1p - cyp ) / ry;
	th = atan2( uy, ux );
	dth = atan2( ux * vy - uy * vx, ux * vx + uy * vy );
	if ( !sweep && dth > 0 )
	{
		dth -= 2 * SVG_PI;
	}
	else if ( sweep && dth < 0 )
	{
		dth += 2 * SVG_PI;
	}
	n = (int)ceil( fabs( dth ) / ( SVG_PI / 2 ) - 1e-9 );
	n = n < 1 ? 1 : n;
	k = 4.0 / 3.0 * tan( dth / n / 4 );
	for ( i = 0; i < n; i++ )
	{
		a0 = th + dth * i / n;
		a1 = th + dth * ( i + 1 ) / n;
		// points and derivatives of the ellipse at the two angles
		e0x = cx + rx * cos( a0 ) * cp - ry * sin( a0 ) * sp;
		e0y = cy + rx * cos( a0 ) * sp + ry * sin( a0 ) * cp;
		e1x = cx + rx * cos( a1 ) * cp - ry * sin( a1 ) * sp;
		e1y = cy + rx * cos( a1 ) * sp + ry * sin( a1 ) * cp;
		d0x = -rx * sin( a0 ) * cp - ry * cos( a0 ) * sp;
		d0y = -rx * sin( a0 ) * sp + ry * cos( a0 ) * cp;
		d1x = -rx * sin( a1 ) * cp - ry * cos( a1 ) * sp;
		d1y = -rx * sin( a1 ) * sp + ry * cos( a1 ) * cp;
		svg_cubic( c, pa, svg_xf( m, e0x, e0y ), svg_xf( m, e0x + k * d0x, e0y + k * d0y ),
				   svg_xf( m, e1x - k * d1x, e1y - k * d1y ), svg_xf( m, e1x, e1y ) );
	}
}

/**
  * @brief  Flatten path data into the outline.
  *
  * @param  c Context.
  * @param  pa The outline.
  * @param  d The "d" attribute.
  * @param  dl Its length.
  * @param  m User to device.
  *
  * @retval None.
  */
static void svg_path_d( svg_ctx *c, svg_path *pa, const char *d, int dl, const svg_mat *m )
{
	static const char cmds[] = "MLHVCSQTAZ";
	static const int nargs[] = { 2, 2, 1, 1, 6, 4, 4, 2, 7, 0 };
	const char *p = d, *e = d + dl, *q;
	double a[7], cx = 0, cy = 0, sx = 0, sy = 0, lx = 0, ly = 0, x1, y1, x2, y2;
	char cmd = 0, up, prev = 0;
	int i, n, rel;

	for ( ;; )
	{
		while ( p < e && ( isspace( (unsigned char)*p ) || *p == ',' ) )
		{
			p++;
		}
		if ( p >= e )
		{
			break;
		}
		if ( isalpha( (unsigned char)*p ) )
		{
			cmd = *p++;
		}
		else if ( !cmd )
		{
			break;
		}
		up = (char)toupper( (unsigned char)cmd );
		rel = cmd != up;
		q = strchr( cmds, up );
		if ( !q )
		{
			break;
		}
		n = nargs[q - cmds];
		for ( i = 0; i < n; i++ )
		{
			if ( up == 'A' && ( i == 3 || i == 4 ) )  // flags: one character, maybe unseparated
			{
				while ( p < e && ( isspace( (unsigned char)*p ) || *p == ',' ) )
				{
					p++;
				}
				if ( p >= e || ( *p != '0' && *p != '1' ) )
				{
					return;
				}
				a[i] = *p++ - '0';
				continue;
			}
			if ( !( q = svg_num( p, e, &a[i] ) ) )
			{
				return;
			}
			p = q;
		}
		if ( rel && up != 'A' && up != 'H' && up != 'V' )
		{
			for ( i = 0; i < n; i++ )
			{
				a[i] += i & 1 ? cy : cx;
			}
		}
		switch ( up )
		{
		case 'M':
			svg_move( c, pa, svg_xf( m, a[0], a[1] ) );
			cx = sx = a[0];
			cy = sy = a[1];
			cmd = rel ? 'l' : 'L';  // further pairs are lines
			break;
		case 'L':
			cx = a[0];
			cy = a[1];
			svg_line( c, pa, svg_xf( m, cx, cy ) );
			break;
		case 'H':
			cx = rel ? cx + a[0] : a[0];
			svg_line( c, pa, svg_xf( m, cx, cy ) );
			break;
		case 'V':
			cy = rel ? cy + a[0] : a[0];
			svg_line( c, pa, svg_xf( m, cx, cy ) );
			break;
		case 'C':
			svg_cubic( c, pa, svg_xf( m, cx, cy ), svg_xf( m, a[0], a[1] ), svg_xf( m, a[2], a[3] ),
					   svg_xf( m, a[4], a[5] ) );
			lx = a[2];
			ly = a[3];
			cx = a[4];
			cy = a[5];
			break;
		case 'S':
			x1 = strchr( "CcSs", prev ) && prev ? 2 * cx - lx : cx;
			y1 = strchr( "CcSs", prev ) && prev ? 2 * cy - ly : cy;
			svg_cubic( c, pa, svg_xf( m, cx, cy ), svg_xf( m, x1, y1 ), svg_xf( m, a[0], a[1] ), svg_xf( m, a[2], a[3] ) );
			lx = a[0];
			ly = a[1];
			cx = a[2];
			cy = a[3];
			break;
		case 'Q':
		case 'T':
			if ( up == 'Q' )
			{
				x1 = a[0];
				y1 = a[1];
				x2 = a[2];
				y2 = a[3];
			}
			else
			{
				x1 = prev && strchr( "QqTt", prev ) ? 2 * cx - lx : cx;
				y1 = prev && strchr( "QqTt", prev ) ? 2 * cy - ly : cy;
				x2 = a[0];
				y2 = a[1];
			}
			svg_cubic( c, pa, svg_xf( m, cx, cy ), svg_xf( m, cx + 2.0 / 3 * ( x1 - cx ), cy + 2.0 / 3 * ( y1 - cy ) ),
					   svg_xf( m, x2 + 2.0 / 3 * ( x1 - x2 ), y2 + 2.0 / 3 * ( y1 - y2 ) ), svg_xf( m, x2, y2 ) );
			lx = x1;
			ly = y1;
			cx = x2;
			cy = y2;
			break;
		case 'A':
			x2 = rel ? cx + a[5] : a[5];
			y2 = rel ? cy + a[6] : a[6];
			svg_arc( c, pa, m, cx, cy, a[0], a[1], a[2], (int)a[3], (int)a[4], x2, y2 );
			cx = x2;
			cy = y2;
			break;
		default:  // Z
			if ( pa->nsub )
			{
				pa->closed[pa->nsub - 1] = 1;
			}
			cx = sx;
			cy = sy;
			prev = cmd;
			cmd = 0;  // a number after Z is an error
			continue;
		}
		prev = up == 'M' ? 'M' : cmd;
	}
}

/**
  * @brief  The points attribute of <polyline> / <polygon>.
  *
  * @param  c Context.
  * @param  pa The outline.
  * @param  v The attribute.
  * @param  vl Its length.
  * @param  m User to device.
  * @param  close 1 for a polygon.
  *
  * @retval None.
  */
static void svg_points( svg_ctx *c, svg_path *pa, const char *v, int vl, const svg_mat *m, int close )
{
	const char *p = v, *e = v + vl, *q;
	double x, y;
	int first = 1;

	while ( ( q = svg_num( p, e, &x ) ) && ( q = svg_num( q, e, &y ) ) )
	{
		p = q;
		if ( first )
		{
			svg_move( c, pa, svg_xf( m, x, y ) );
			first = 0;
		}
		else
		{
			svg_line( c, pa, svg_xf( m, x, y ) );
		}
	}
	if ( close && pa->nsub )
	{
		pa->closed[pa->nsub - 1] = 1;
	}
}

/**
  * @brief  A closed ellipse as four arcs.
  *
  * @param  c Context.
  * @param  pa The outline.
  * @param  m User to device.
  * @param  cx Centre x.
  * @param  cy Centre y.
  * @param  rx Radius x.
  * @param  ry Radius y.
  *
  * @retval None.
  */
static void svg_ellipse( svg_ctx *c, svg_path *pa, const svg_mat *m, double cx, double cy, double rx, double ry )
{
	if ( rx <= 0 || ry <= 0 )
	{
		return;
	}
	svg_move( c, pa, svg_xf( m, cx + rx, cy ) );
	svg_arc( c, pa, m, cx + rx, cy, rx, ry, 0, 0, 1, cx, cy + ry );
	svg_arc( c, pa, m, cx, cy + ry, rx, ry, 0, 0, 1, cx - rx, cy );
	svg_arc( c, pa, m, cx - rx, cy, rx, ry, 0, 0, 1, cx, cy - ry );
	svg_arc( c, pa, m, cx, cy - ry, rx, ry, 0, 0, 1, cx + rx, cy );
	pa->closed[pa->nsub - 1] = 1;
}

/**
  * @brief  A numeric attribute.
  *
  * @param  t The element.
  * @param  name Attribute name.
  * @param  def Value when missing.
  *
  * @retval The number.
  */
static double svg_attr_num( const svg_tag *t, const char *name, double def )
{
	const char *v;
	double x;
	int vl;

	return svg_attr( t, name, &v, &vl ) && svg_num( v, v + vl, &x ) ? x : def;
}

/**
  * @brief  Build the outline of a shape element.
  *
  * @param  c Context.
  * @param  pa The outline (emptied first).
  * @param  t The element.
  * @param  m User to device (with the element's own transform).
  *
  * @retval 1 it is a shape, 0 it is not.
  */
static int svg_shape( svg_ctx *c, svg_path *pa, const svg_tag *t, const svg_mat *m )
{
	const char *v;
	double x, y, w, h, rx, ry;
	int vl;

	pa->n = pa->nsub = 0;
	if ( svg_is( t, "path" ) )
	{
		if ( svg_attr( t, "d", &v, &vl ) )
		{
			svg_path_d( c, pa, v, vl, m );
		}
	}
	else if ( svg_is( t, "rect" ) )
	{
		x = svg_attr_num( t, "x", 0 );
		y = svg_attr_num( t, "y", 0 );
		w = svg_attr_num( t, "width", 0 );
		h = svg_attr_num( t, "height", 0 );
		rx = svg_attr_num( t, "rx", -1 );
		ry = svg_attr_num( t, "ry", -1 );
		rx = rx < 0 ? ( ry < 0 ? 0 : ry ) : rx;
		ry = ry < 0 ? rx : ry;
		rx = rx > w / 2 ? w / 2 : rx;
		ry = ry > h / 2 ? h / 2 : ry;
		if ( w <= 0 || h <= 0 )
		{
			return 1;
		}
		svg_move( c, pa, svg_xf( m, x + rx, y ) );
		svg_line( c, pa, svg_xf( m, x + w - rx, y ) );
		svg_arc( c, pa, m, x + w - rx, y, rx, ry, 0, 0, 1, x + w, y + ry );
		svg_line( c, pa, svg_xf( m, x + w, y + h - ry ) );
		svg_arc( c, pa, m, x + w, y + h - ry, rx, ry, 0, 0, 1, x + w - rx, y + h );
		svg_line( c, pa, svg_xf( m, x + rx, y + h ) );
		svg_arc( c, pa, m, x + rx, y + h, rx, ry, 0, 0, 1, x, y + h - ry );
		svg_line( c, pa, svg_xf( m, x, y + ry ) );
		svg_arc( c, pa, m, x, y + ry, rx, ry, 0, 0, 1, x + rx, y );
		pa->closed[pa->nsub - 1] = 1;
	}
	else if ( svg_is( t, "circle" ) )
	{
		rx = svg_attr_num( t, "r", 0 );
		svg_ellipse( c, pa, m, svg_attr_num( t, "cx", 0 ), svg_attr_num( t, "cy", 0 ), rx, rx );
	}
	else if ( svg_is( t, "ellipse" ) )
	{
		svg_ellipse( c, pa, m, svg_attr_num( t, "cx", 0 ), svg_attr_num( t, "cy", 0 ), svg_attr_num( t, "rx", 0 ),
					 svg_attr_num( t, "ry", 0 ) );
	}
	else if ( svg_is( t, "line" ) )
	{
		svg_move( c, pa, svg_xf( m, svg_attr_num( t, "x1", 0 ), svg_attr_num( t, "y1", 0 ) ) );
		svg_line( c, pa, svg_xf( m, svg_attr_num( t, "x2", 0 ), svg_attr_num( t, "y2", 0 ) ) );
	}
	else if ( svg_is( t, "polyline" ) || svg_is( t, "polygon" ) )
	{
		if ( svg_attr( t, "points", &v, &vl ) )
		{
			svg_points( c, pa, v, vl, m, svg_is( t, "polygon" ) );
		}
	}
	else
	{
		return 0;
	}
	return 1;
}

/**
  * @brief  Add an edge and extend the bounding box.
  *
  * @param  c Context.
  * @param  ed The edge list.
  * @param  a From.
  * @param  b To.
  *
  * @retval None.
  */
static void svg_edge_add( svg_ctx *c, svg_edges *ed, svg_pt a, svg_pt b )
{
	if ( a.y == b.y )
	{
		// horizontal edges add no area, but still bound the shape
	}
	else if ( svg_grow( (void **)&ed->e, &ed->cap, ed->n + 1, sizeof *ed->e ) )
	{
		c->oom = 1;
		return;
	}
	else
	{
		ed->e[ed->n++] = ( svg_edge ){ a.x, a.y, b.x, b.y };
	}
	ed->minx = fminf( ed->minx, fminf( a.x, b.x ) );
	ed->maxx = fmaxf( ed->maxx, fmaxf( a.x, b.x ) );
	ed->miny = fminf( ed->miny, fminf( a.y, b.y ) );
	ed->maxy = fmaxf( ed->maxy, fmaxf( a.y, b.y ) );
}

/**
  * @brief  Add a closed polygon wound positively, whatever order its points are in.
  *
  * Stroke pieces must all wind the same way, so that overlaps add up
  * (and clamp) rather than cancel.
  *
  * @param  c Context.
  * @param  ed The edge list.
  * @param  p The points.
  * @param  n How many.
  *
  * @retval None.
  */
static void svg_poly( svg_ctx *c, svg_edges *ed, const svg_pt *p, int n )
{
	float area = 0;
	int i;

	for ( i = 0; i < n; i++ )
	{
		area += p[i].x * p[( i + 1 ) % n].y - p[( i + 1 ) % n].x * p[i].y;
	}
	for ( i = 0; i < n; i++ )
	{
		if ( area >= 0 )
		{
			svg_edge_add( c, ed, p[i], p[( i + 1 ) % n] );
		}
		else
		{
			svg_edge_add( c, ed, p[( i + 1 ) % n], p[i] );
		}
	}
}

/**
  * @brief  A filled circle (round caps and joins).
  *
  * @param  c Context.
  * @param  ed The edge list.
  * @param  o Centre.
  * @param  r Radius in pixels.
  *
  * @retval None.
  */
static void svg_circle_poly( svg_ctx *c, svg_edges *ed, svg_pt o, float r )
{
	svg_pt p[64];
	int n = 8 + (int)( r * 2 ), i;

	n = n > 64 ? 64 : n;
	for ( i = 0; i < n; i++ )
	{
		p[i].x = o.x + r * cosf( (float)( 2 * SVG_PI * i / n ) );
		p[i].y = o.y + r * sinf( (float)( 2 * SVG_PI * i / n ) );
	}
	svg_poly( c, ed, p, n );
}

/**
  * @brief  Edges of the outline's interior (every subpath closed).
  *
  * @param  c Context.
  * @param  pa The outline.
  * @param  ed Receives the edges.
  *
  * @retval None.
  */
static void svg_fill_edges( svg_ctx *c, const svg_path *pa, svg_edges *ed )
{
	int s, i, a, b;

	for ( s = 0; s < pa->nsub; s++ )
	{
		a = pa->sub[s];
		b = s + 1 < pa->nsub ? pa->sub[s + 1] : pa->n;
		for ( i = a; i < b; i++ )
		{
			svg_edge_add( c, ed, pa->p[i], pa->p[i + 1 < b ? i + 1 : a] );
		}
	}
}

/**
  * @brief  Edges of the outline's stroke: a quad per segment, joins, caps.
  *
  * @param  c Context.
  * @param  pa The outline (repeated points are removed from it).
  * @param  ed Receives the edges.
  * @param  s The style (caps, joins, miter limit).
  * @param  hw Half the stroke width, in pixels.
  *
  * @retval None.
  */
static void svg_stroke_edges( svg_ctx *c, svg_path *pa, svg_edges *ed, const svg_style *s, float hw )
{
	svg_pt q[4], v, d1, d2, n1, n2, *pts;
	float l, cr, co, k;
	int sub, a, b, n, i, j, closed, nseg;

	for ( sub = 0; sub < pa->nsub; sub++ )
	{
		a = pa->sub[sub];
		b = sub + 1 < pa->nsub ? pa->sub[sub + 1] : pa->n;
		closed = pa->closed[sub];
		// drop repeated points: they have no direction
		pts = pa->p + a;
		for ( n = 0, i = 0; i < b - a; i++ )
		{
			if ( !n || fabsf( pts[i].x - pts[n - 1].x ) + fabsf( pts[i].y - pts[n - 1].y ) > 1e-4f )
			{
				pts[n++] = pts[i];
			}
		}
		if ( closed && n > 1 && fabsf( pts[0].x - pts[n - 1].x ) + fabsf( pts[0].y - pts[n - 1].y ) <= 1e-4f )
		{
			n--;
		}
		if ( n == 1 )
		{
			if ( s->cap == SVG_CAP_ROUND )
			{
				svg_circle_poly( c, ed, pts[0], hw );
			}
			continue;
		}
		nseg = closed ? n : n - 1;
		for ( i = 0; i < nseg; i++ )
		{
			v = pts[i];
			d1.x = pts[( i + 1 ) % n].x - v.x;
			d1.y = pts[( i + 1 ) % n].y - v.y;
			l = hypotf( d1.x, d1.y );
			n1.x = -d1.y / l * hw;
			n1.y = d1.x / l * hw;
			q[0] = ( svg_pt ){ v.x + n1.x, v.y + n1.y };
			q[1] = ( svg_pt ){ v.x + d1.x + n1.x, v.y + d1.y + n1.y };
			q[2] = ( svg_pt ){ v.x + d1.x - n1.x, v.y + d1.y - n1.y };
			q[3] = ( svg_pt ){ v.x - n1.x, v.y - n1.y };
			svg_poly( c, ed, q, 4 );
		}
		// joins at every vertex between two segments
		for ( j = closed ? 0 : 1; j < ( closed ? n : n - 1 ); j++ )
		{
			v = pts[j];
			i = ( j + n - 1 ) % n;
			d1.x = v.x - pts[i].x;
			d1.y = v.y - pts[i].y;
			l = hypotf( d1.x, d1.y );
			d1.x /= l;
			d1.y /= l;
			d2.x = pts[( j + 1 ) % n].x - v.x;
			d2.y = pts[( j + 1 ) % n].y - v.y;
			l = hypotf( d2.x, d2.y );
			d2.x /= l;
			d2.y /= l;
			cr = d1.x * d2.y - d1.y * d2.x;
			co = d1.x * d2.x + d1.y * d2.y;
			if ( fabsf( cr ) < 1e-6f && co > 0 )
			{
				continue;  // straight on
			}
			if ( s->join == SVG_JOIN_ROUND )
			{
				svg_circle_poly( c, ed, v, hw );
				continue;
			}
			// the gap is on the outer side: turning towards +n puts it at -n
			k = cr > 0 ? -hw : hw;
			n1 = ( svg_pt ){ -d1.y * k, d1.x * k };
			n2 = ( svg_pt ){ -d2.y * k, d2.x * k };
			q[0] = v;
			q[1] = ( svg_pt ){ v.x + n1.x, v.y + n1.y };
			if ( s->join == SVG_JOIN_MITER && 1 + co > 1e-6f && sqrtf( 2 / ( 1 + co ) ) <= s->miter )
			{
				q[2] = ( svg_pt ){ v.x + ( n1.x + n2.x ) / ( 1 + co ), v.y + ( n1.y + n2.y ) / ( 1 + co ) };
				q[3] = ( svg_pt ){ v.x + n2.x, v.y + n2.y };
				svg_poly( c, ed, q, 4 );
			}
			else
			{
				q[2] = ( svg_pt ){ v.x + n2.x, v.y + n2.y };
				svg_poly( c, ed, q, 3 );
			}
		}
		if ( closed || s->cap == SVG_CAP_BUTT )
		{
			continue;
		}
		for ( j = 0; j < 2; j++ )  // caps at both ends
		{
			v = j ? pts[n - 1] : pts[0];
			d1.x = j ? v.x - pts[n - 2].x : v.x - pts[1].x;  // pointing out of the line
			d1.y = j ? v.y - pts[n - 2].y : v.y - pts[1].y;
			if ( s->cap == SVG_CAP_ROUND )
			{
				svg_circle_poly( c, ed, v, hw );
				continue;
			}
			l = hypotf( d1.x, d1.y );
			d1.x = d1.x / l * hw;
			d1.y = d1.y / l * hw;
			q[0] = ( svg_pt ){ v.x - d1.y, v.y + d1.x };
			q[1] = ( svg_pt ){ v.x - d1.y + d1.x, v.y + d1.x + d1.y };
			q[2] = ( svg_pt ){ v.x + d1.y + d1.x, v.y - d1.x + d1.y };
			q[3] = ( svg_pt ){ v.x + d1.y, v.y - d1.x };
			svg_poly( c, ed, q, 4 );
		}
	}
}

/**
  * @brief  Accumulate the signed area of one edge, x already within [0, w].
  *
  * @param  acc Rows of w + 2 cells.
  * @param  w Region width.
  * @param  h Region height.
  * @param  x0 From x.
  * @param  y0 From y.
  * @param  x1 To x.
  * @param  y1 To y.
  *
  * @retval None.
  */
static void svg_acc_line( float *acc, int w, int h, float x0, float y0, float x1, float y1 )
{
	float dir = 1, dxdy, x, dy, xn, d, xa, xb, xaf, s, xf0, xf1, a0, am, a1, a2, t;
	int y, ys, ye, i0, i1, i;
	float *row;

	if ( y0 == y1 )
	{
		return;
	}
	if ( y0 > y1 )
	{
		t = x0; x0 = x1; x1 = t;
		t = y0; y0 = y1; y1 = t;
		dir = -1;
	}
	if ( y1 <= 0 || y0 >= h )
	{
		return;
	}
	dxdy = ( x1 - x0 ) / ( y1 - y0 );
	x = x0;
	if ( y0 < 0 )
	{
		x -= y0 * dxdy;
		ys = 0;
	}
	else
	{
		ys = (int)y0;
	}
	x = x < 0 ? 0 : x > w ? (float)w : x;
	ye = (int)ceilf( y1 );
	ye = ye > h ? h : ye;
	for ( y = ys; y < ye; y++ )
	{
		row = acc + (size_t)y * ( w + 2 );
		dy = fminf( (float)( y + 1 ), y1 ) - fmaxf( (float)y, y0 );
		xn = x + dxdy * dy;
		xn = xn < 0 ? 0 : xn > w ? (float)w : xn;
		d = dy * dir;
		xa = fminf( x, xn );
		xb = fmaxf( x, xn );
		xaf = floorf( xa );
		i0 = (int)xaf;
		i1 = (int)ceilf( xb );
		if ( i1 <= i0 + 1 )
		{
			// within one cell: split by the mean x
			t = 0.5f * ( x + xn ) - xaf;
			row[i0] += d - d * t;
			row[i0 + 1] += d * t;
		}
		else
		{
			s = 1 / ( xb - xa );
			xf0 = xa - xaf;
			a0 = 0.5f * s * ( 1 - xf0 ) * ( 1 - xf0 );
			xf1 = xb - (float)i1 + 1;
			am = 0.5f * s * xf1 * xf1;
			row[i0] += d * a0;
			if ( i1 == i0 + 2 )
			{
				row[i0 + 1] += d * ( 1 - a0 - am );
			}
			else
			{
				a1 = s * ( 1.5f - xf0 );
				row[i0 + 1] += d * ( a1 - a0 );
				for ( i = i0 + 2; i < i1 - 1; i++ )
				{
					row[i] += d * s;
				}
				a2 = a1 + (float)( i1 - i0 - 3 ) * s;
				row[i1 - 1] += d * ( 1 - a2 - am );
			}
			row[i1] += d * am;
		}
		x = xn;
	}
}

/**
  * @brief  Accumulate an edge, cut to the region's columns.
  *
  * The part right of the region changes nothing inside it and is dropped;
  * the part left of it still changes the winding of every pixel in the
  * row, so it is moved onto the left border.
  *
  * @param  acc Rows of w + 2 cells.
  * @param  w Region width.
  * @param  h Region height.
  * @param  x0 From x.
  * @param  y0 From y.
  * @param  x1 To x.
  * @param  y1 To y.
  *
  * @retval None.
  */
static void svg_acc_edge( float *acc, int w, int h, float x0, float y0, float x1, float y1 )
{
	float ym;

	if ( x0 >= w && x1 >= w )
	{
		return;
	}
	if ( x0 > w || x1 > w )
	{
		ym = y0 + ( (float)w - x0 ) / ( x1 - x0 ) * ( y1 - y0 );
		if ( x0 > w )
		{
			x0 = (float)w;
			y0 = ym;
		}
		else
		{
			x1 = (float)w;
			y1 = ym;
		}
	}
	if ( x0 <= 0 && x1 <= 0 )
	{
		svg_acc_line( acc, w, h, 0, y0, 0, y1 );
		return;
	}
	if ( x0 < 0 || x1 < 0 )
	{
		ym = y0 + ( 0 - x0 ) / ( x1 - x0 ) * ( y1 - y0 );
		if ( x0 < 0 )
		{
			svg_acc_line( acc, w, h, 0, y0, 0, ym );
			svg_acc_line( acc, w, h, 0, ym, x1, y1 );
		}
		else
		{
			svg_acc_line( acc, w, h, x0, y0, 0, ym );
			svg_acc_line( acc, w, h, 0, ym, 0, y1 );
		}
		return;
	}
	svg_acc_line( acc, w, h, x0, y0, x1, y1 );
}

/**
  * @brief  Coverage of an edge list over a region (non-zero rule).
  *
  * @param  c Context (accumulation buffer).
  * @param  ed The edges, device pixels.
  * @param  rx Region left.
  * @param  ry Region top.
  * @param  rw Region width.
  * @param  rh Region height.
  * @param  cov Receives rw x rh values 0..1.
  *
  * @retval None.
  */
static void svg_cover( svg_ctx *c, const svg_edges *ed, int rx, int ry, int rw, int rh, float *cov )
{
	float *acc = c->acc, sum, v;
	int i, x, y;

	memset( acc, 0, (size_t)( rw + 2 ) * (size_t)rh * sizeof *acc );
	for ( i = 0; i < ed->n; i++ )
	{
		svg_acc_edge( acc, rw, rh, ed->e[i].x0 - (float)rx, ed->e[i].y0 - (float)ry, ed->e[i].x1 - (float)rx,
					  ed->e[i].y1 - (float)ry );
	}
	for ( y = 0; y < rh; y++ )
	{
		sum = 0;
		for ( x = 0; x < rw; x++ )
		{
			sum += acc[(size_t)y * ( rw + 2 ) + x];
			v = fabsf( sum );
			cov[(size_t)y * rw + x] = v > 1 ? 1 : v;
		}
	}
}

/**
  * @brief  Build a clip's edges (all its shapes) into c->ced.
  *
  * @param  c Context.
  * @param  cl The clip.
  *
  * @retval 1 built, 0 empty.
  */
static int svg_clip_edges( svg_ctx *c, const svg_clip *cl )
{
	const char *p = cl->def->body;
	svg_tag t;
	svg_mat m;
	const char *v;
	int vl;

	c->ced.n = 0;
	c->ced.minx = c->ced.miny = 1e30f;
	c->ced.maxx = c->ced.maxy = -1e30f;
	while ( p < cl->def->end && svg_next_tag( &p, cl->def->end, &t ) )
	{
		if ( t.kind == SVG_TAG_CLOSE )
		{
			continue;
		}
		m = svg_mul( cl->m, cl->def->m );
		if ( svg_attr( &t, "transform", &v, &vl ) )
		{
			svg_transform( v, vl, &m );
		}
		if ( svg_shape( c, &c->cpath, &t, &m ) )
		{
			svg_fill_edges( c, &c->cpath, &c->ced );
		}
	}
	return c->ced.maxx >= c->ced.minx;
}

/**
  * @brief  Paint edges in a colour, through the active clips.
  *
  * @param  c Context.
  * @param  ed The edges.
  * @param  rgb Colour 0xRRGGBB.
  * @param  alpha Opacity 0..1.
  *
  * @retval None.
  */
static void svg_paint( svg_ctx *c, const svg_edges *ed, uint32_t rgb, float alpha )
{
	int x0, y0, x1, y1, rw, rh, i, k, x, y;
	size_t need;
	uint32_t *px, o;
	float a, cr = (float)( ( rgb >> 16 ) & 0xFF ), cg = (float)( ( rgb >> 8 ) & 0xFF ), cb = (float)( rgb & 0xFF );
	void *q;

	if ( alpha <= 0 || ed->maxx < ed->minx )
	{
		return;
	}
	x0 = (int)floorf( ed->minx );
	y0 = (int)floorf( ed->miny );
	x1 = (int)ceilf( ed->maxx );
	y1 = (int)ceilf( ed->maxy );
	x0 = x0 < 0 ? 0 : x0;
	y0 = y0 < 0 ? 0 : y0;
	x1 = x1 > c->w ? c->w : x1;
	y1 = y1 > c->h ? c->h : y1;
	if ( x1 <= x0 || y1 <= y0 )
	{
		return;
	}
	rw = x1 - x0;
	rh = y1 - y0;
	need = (size_t)( rw + 2 ) * (size_t)rh;
	if ( need > c->acccap )
	{
		if ( !( q = realloc( c->acc, need * sizeof *c->acc ) ) )
		{
			c->oom = 1;
			return;
		}
		c->acc = q;
		c->acccap = need;
	}
	need = (size_t)rw * (size_t)rh;
	if ( need > c->covcap )
	{
		if ( !( q = realloc( c->cov, need * sizeof *c->cov ) ) )
		{
			c->oom = 1;
			return;
		}
		c->cov = q;
		if ( !( q = realloc( c->cov2, need * sizeof *c->cov2 ) ) )
		{
			c->oom = 1;
			return;
		}
		c->cov2 = q;
		c->covcap = need;
	}
	svg_cover( c, ed, x0, y0, rw, rh, c->cov );
	for ( k = 0; k < c->nclips; k++ )
	{
		if ( !svg_clip_edges( c, &c->clips[k] ) )
		{
			return;
		}
		svg_cover( c, &c->ced, x0, y0, rw, rh, c->cov2 );
		for ( i = 0; i < rw * rh; i++ )
		{
			c->cov[i] *= c->cov2[i];
		}
	}
	for ( y = 0; y < rh; y++ )
	{
		px = c->out + (size_t)( y0 + y ) * c->w + x0;
		for ( x = 0; x < rw; x++ )
		{
			a = c->cov[(size_t)y * rw + x] * alpha;
			if ( a <= 0.002f )
			{
				continue;
			}
			o = px[x];
			px[x] = 0xFF000000u |
					(uint32_t)( (float)( ( o >> 16 ) & 0xFF ) * ( 1 - a ) + cr * a + 0.5f ) << 16 |
					(uint32_t)( (float)( ( o >> 8 ) & 0xFF ) * ( 1 - a ) + cg * a + 0.5f ) << 8 |
					(uint32_t)( (float)( o & 0xFF ) * ( 1 - a ) + cb * a + 0.5f );
		}
	}
}

/**
  * @brief  Find a clip definition from a clip-path value ("url(#id)").
  *
  * @param  c Context.
  * @param  v The value.
  * @param  vl Its length.
  *
  * @retval The definition, NULL if none.
  */
static const svg_clipdef *svg_find_clip( svg_ctx *c, const char *v, int vl )
{
	const char *h = memchr( v, '#', (size_t)vl ), *e;
	int i, n;

	if ( !h )
	{
		return NULL;
	}
	h++;
	for ( e = h; e < v + vl && *e != ')' && *e != '\'' && *e != '"'; e++ )
	{
	}
	n = (int)( e - h );
	for ( i = 0; i < c->ndefs; i++ )
	{
		if ( c->defs[i].idlen == n && !memcmp( c->defs[i].id, h, (size_t)n ) )
		{
			return &c->defs[i];
		}
	}
	return NULL;
}

/**
  * @brief  Paint one shape element.
  *
  * @param  c Context.
  * @param  t The element.
  * @param  st The enclosing group's state.
  *
  * @retval None.
  */
static void svg_draw( svg_ctx *c, const svg_tag *t, const svg_state *st )
{
	svg_mat m = st->m;
	svg_style s = st->s;
	const svg_clipdef *def;
	const char *v;
	float hw;
	int vl, nclips = c->nclips;

	if ( svg_attr( t, "transform", &v, &vl ) )
	{
		svg_transform( v, vl, &m );
	}
	svg_style_of( t, &s );
	if ( s.hidden || ( !s.do_fill && !s.do_stroke ) )
	{
		return;
	}
	if ( svg_attr( t, "clip-path", &v, &vl ) && ( def = svg_find_clip( c, v, vl ) ) && c->nclips < SVG_MAX_CLIPS )
	{
		c->clips[c->nclips++] = ( svg_clip ){ def, m };
	}
	if ( !svg_shape( c, &c->path, t, &m ) )
	{
		c->nclips = nclips;
		return;
	}
	if ( s.do_fill )
	{
		c->ed.n = 0;
		c->ed.minx = c->ed.miny = 1e30f;
		c->ed.maxx = c->ed.maxy = -1e30f;
		svg_fill_edges( c, &c->path, &c->ed );
		svg_paint( c, &c->ed, s.fill, s.fill_op * s.op );
	}
	hw = (float)( s.width * sqrt( fabs( m.a * m.d - m.b * m.c ) ) / 2 );
	if ( s.do_stroke && hw > 0 )
	{
		c->ed.n = 0;
		c->ed.minx = c->ed.miny = 1e30f;
		c->ed.maxx = c->ed.maxy = -1e30f;
		svg_stroke_edges( c, &c->path, &c->ed, &s, hw );
		svg_paint( c, &c->ed, s.stroke, s.stroke_op * s.op );
	}
	c->nclips = nclips;
}

/**
  * @brief  First pass: remember every <clipPath> by id.
  *
  * @param  c Context.
  * @param  doc The text.
  * @param  end Its end.
  *
  * @retval 0 done, -2 out of memory.
  */
static int svg_collect_clips( svg_ctx *c, const char *doc, const char *end )
{
	const char *p = doc, *body, *v;
	svg_clipdef *d;
	svg_tag t;
	int vl;

	while ( svg_next_tag( &p, end, &t ) )
	{
		if ( t.kind != SVG_TAG_OPEN || !svg_is( &t, "clipPath" ) || !svg_attr( &t, "id", &v, &vl ) )
		{
			continue;
		}
		if ( svg_grow( (void **)&c->defs, &c->defcap, c->ndefs + 1, sizeof *c->defs ) )
		{
			return -2;
		}
		d = &c->defs[c->ndefs++];
		d->id = v;
		d->idlen = vl;
		d->m = ( svg_mat ){ 1, 0, 0, 1, 0, 0 };
		if ( svg_attr( &t, "transform", &v, &vl ) )
		{
			svg_transform( v, vl, &d->m );
		}
		d->body = body = p;
		while ( svg_next_tag( &p, end, &t ) && !( t.kind == SVG_TAG_CLOSE && svg_is( &t, "clipPath" ) ) )
		{
			body = p;
		}
		d->end = body;
	}
	return 0;
}

//------------------------------------------------------------------------------
//------------------------------ Public API ------------------------------------
//------------------------------------------------------------------------------

/**
  * @brief  The document's size in user units (its viewBox, else width / height).
  *
  * @param  doc The SVG text.
  * @param  len Its length.
  * @param  w Receives the width.
  * @param  h Receives the height.
  *
  * @retval 0 found, -1 not an SVG document.
  */
int svg_size( const char *doc, size_t len, double *w, double *h )
{
	const char *p = doc, *e = doc + len, *v, *q;
	double vb[4];
	svg_tag t;
	int vl, i;

	while ( svg_next_tag( &p, e, &t ) )
	{
		if ( t.kind == SVG_TAG_CLOSE || !svg_is( &t, "svg" ) )
		{
			continue;
		}
		if ( svg_attr( &t, "viewBox", &v, &vl ) )
		{
			for ( q = v, i = 0; i < 4 && ( q = svg_num( q, v + vl, &vb[i] ) ); i++ )
			{
			}
			if ( i == 4 && vb[2] > 0 && vb[3] > 0 )
			{
				*w = vb[2];
				*h = vb[3];
				return 0;
			}
		}
		*w = svg_attr_num( &t, "width", 0 );
		*h = svg_attr_num( &t, "height", 0 );
		return *w > 0 && *h > 0 ? 0 : -1;
	}
	return -1;
}

/**
  * @brief  Render the document over out[], scaling its viewBox to w x h pixels.
  *
  * @param  doc The SVG text.
  * @param  len Its length.
  * @param  out w x h pixels, 0xAARRGGBB; drawn over what is there.
  * @param  w Width.
  * @param  h Height.
  *
  * @retval 0 rendered, -1 not an SVG document, -2 out of memory.
  */
int svg_render( const char *doc, size_t len, uint32_t *out, int w, int h )
{
	static const svg_style base = { 0x000000, 0x000000, 1, 0, 0, 1, 1, 1, 1, 4, SVG_CAP_BUTT, SVG_JOIN_MITER };
	svg_state stack[SVG_DEPTH];
	const char *p = doc, *e = doc + len, *v, *q;
	const svg_clipdef *def;
	svg_ctx c;
	svg_tag t;
	double vb[4] = { 0, 0, 0, 0 };
	int sp = 0, skip = 0, root = 0, vl, i, rc;

	memset( &c, 0, sizeof c );
	c.out = out;
	c.w = w;
	c.h = h;
	if ( svg_size( doc, len, &vb[2], &vb[3] ) )
	{
		return -1;
	}
	rc = svg_collect_clips( &c, doc, e );
	while ( !rc && !c.oom && svg_next_tag( &p, e, &t ) )
	{
		if ( skip )
		{
			skip += t.kind == SVG_TAG_OPEN ? 1 : t.kind == SVG_TAG_CLOSE ? -1 : 0;
			continue;
		}
		if ( t.kind == SVG_TAG_CLOSE )
		{
			if ( sp > 0 )
			{
				c.nclips = stack[--sp].nclip;
			}
			continue;
		}
		if ( svg_is( &t, "defs" ) || svg_is( &t, "clipPath" ) || svg_is( &t, "mask" ) || svg_is( &t, "pattern" ) ||
			 svg_is( &t, "marker" ) || svg_is( &t, "symbol" ) || svg_is( &t, "linearGradient" ) ||
			 svg_is( &t, "radialGradient" ) || svg_is( &t, "filter" ) || svg_is( &t, "metadata" ) ||
			 svg_is( &t, "title" ) || svg_is( &t, "desc" ) || svg_is( &t, "style" ) || svg_is( &t, "text" ) ||
			 svg_is( &t, "sodipodi:namedview" ) )
		{
			skip = t.kind == SVG_TAG_OPEN;
			continue;
		}
		if ( sp == 0 && !root )  // the root <svg>: viewBox to pixels
		{
			if ( !svg_is( &t, "svg" ) )
			{
				continue;
			}
			root = 1;
			if ( svg_attr( &t, "viewBox", &v, &vl ) )
			{
				for ( q = v, i = 0; i < 2 && ( q = svg_num( q, v + vl, &vb[i] ) ); i++ )
				{
				}
			}
			stack[0].m = ( svg_mat ){ w / vb[2], 0, 0, h / vb[3], -vb[0] * w / vb[2], -vb[1] * h / vb[3] };
			stack[0].s = base;
			svg_style_of( &t, &stack[0].s );
			stack[0].nclip = 0;
			sp = 1;
			continue;
		}
		if ( sp == 0 )
		{
			continue;
		}
		if ( svg_is( &t, "path" ) || svg_is( &t, "rect" ) || svg_is( &t, "circle" ) || svg_is( &t, "ellipse" ) ||
			 svg_is( &t, "line" ) || svg_is( &t, "polyline" ) || svg_is( &t, "polygon" ) )
		{
			svg_draw( &c, &t, &stack[sp - 1] );
			if ( t.kind == SVG_TAG_OPEN )  // children (title, animate) are not drawn
			{
				skip = 1;
			}
			continue;
		}
		if ( t.kind == SVG_TAG_EMPTY || sp >= SVG_DEPTH )
		{
			if ( t.kind == SVG_TAG_OPEN )
			{
				skip = 1;
			}
			continue;
		}
		// a group (or any other container): its transform, style and clip apply to the children
		stack[sp] = stack[sp - 1];
		stack[sp].nclip = c.nclips;
		if ( svg_attr( &t, "transform", &v, &vl ) )
		{
			svg_transform( v, vl, &stack[sp].m );
		}
		svg_style_of( &t, &stack[sp].s );
		if ( svg_attr( &t, "clip-path", &v, &vl ) && ( def = svg_find_clip( &c, v, vl ) ) && c.nclips < SVG_MAX_CLIPS )
		{
			c.clips[c.nclips++] = ( svg_clip ){ def, stack[sp].m };
		}
		sp++;
	}
	rc = rc ? rc : c.oom ? -2 : 0;
	free( c.defs );
	free( c.acc );
	free( c.cov );
	free( c.cov2 );
	free( c.path.p );
	free( c.path.sub );
	free( c.path.closed );
	free( c.cpath.p );
	free( c.cpath.sub );
	free( c.cpath.closed );
	free( c.ed.e );
	free( c.ced.e );
	return rc;
}
