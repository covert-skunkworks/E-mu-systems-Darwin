//  Description:
//
//		svg.h -- a small SVG rasterizer for the panel artwork (UI.svg).
//
//		Covers what Inkscape writes for flat artwork: <path>, <rect>,
//		<circle>, <ellipse>, <line>, <polyline>, <polygon> inside <g>
//		groups; transform (matrix, translate, scale, rotate); solid fill and
//		stroke with their opacities (style or attributes); butt / round /
//		square caps, miter / round / bevel joins; clip-path (userSpaceOnUse).
//		Not covered: text (convert it to paths), gradients, masks,
//		filters, dashes. The fill rule is always non-zero.
//

#ifndef SVG_H
#define SVG_H

#include <stddef.h>
#include <stdint.h>

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
int svg_size( const char *doc, size_t len, double *w, double *h );

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
int svg_render( const char *doc, size_t len, uint32_t *out, int w, int h );

#endif
