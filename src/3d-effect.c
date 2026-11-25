#include "3d-effect.h"
#include "version.h"
#include <obs-module.h>
#include "graphics/math-defs.h"
#include "graphics/matrix4.h"
#include "graphics/vec4.h"

#define MODE_PERSPECTIVE 0
#define MODE_ORTHOGRAPHIC 1

float (*move_get_transition_filter)(obs_source_t *filter_from, obs_source_t **filter_to) = NULL;

struct effect_3d_transform {
	float fov;
	struct vec3 position;
	struct vec3 rotation;
	struct vec2 scale;
	struct vec2 shear;
};

struct effect_3d {
	obs_source_t *source;
	int mode;
	struct effect_3d_transform transform;

	bool keep_size;   // FOV no cambia el tamaño en reposo; posición en px reales
	float margin;     // % de la fuente agregado a cada lado para que el giro no se corte
	bool supersample; // dibuja al doble de resolución y reduce: orillas suavizadas
	bool hide_back;   // no dibuja cuando se ve la cara trasera
	bool direct;      // dibuja directo en la escena (como BFX): nada se corta y el margen sobra

	bool processed_frame;
	gs_texrender_t *render;
	enum gs_color_space space;
};

static const char *effect_3d_get_name(void *type_data)
{
	UNUSED_PARAMETER(type_data);
	return obs_module_text("3DEffect");
}

void effect_3d_update(void *data, obs_data_t *settings)
{
	struct effect_3d *context = data;
	struct effect_3d_transform *t = &context->transform;
	context->mode = (int)obs_data_get_int(settings, "mode");
	t->fov = (float)obs_data_get_double(settings, "fov");
	t->rotation.x = (float)obs_data_get_double(settings, "rot_x");
	t->rotation.y = (float)obs_data_get_double(settings, "rot_y");
	t->rotation.z = (float)obs_data_get_double(settings, "rot_z");

	t->position.x = (float)obs_data_get_double(settings, "pos_x");
	t->position.y = (float)obs_data_get_double(settings, "pos_y");
	t->position.z = (float)obs_data_get_double(settings, "pos_z");

	t->scale.x = (float)obs_data_get_double(settings, "scale_x") / 100.0f;
	t->scale.y = (float)obs_data_get_double(settings, "scale_y") / 100.0f;

	t->shear.x = (float)obs_data_get_double(settings, "shear_x") / 100.0f;
	t->shear.y = (float)obs_data_get_double(settings, "shear_y") / 100.0f;

	context->keep_size = obs_data_get_bool(settings, "keep_size");
	context->margin = (float)obs_data_get_double(settings, "margin");
	if (context->margin < 0.0f)
		context->margin = 0.0f;
	context->supersample = obs_data_get_bool(settings, "supersample");
	context->hide_back = obs_data_get_bool(settings, "hide_back");
	context->direct = obs_data_get_bool(settings, "direct");
}

static void *effect_3d_create(obs_data_t *settings, obs_source_t *source)
{
	struct effect_3d *context = bzalloc(sizeof(struct effect_3d));
	context->source = source;
	effect_3d_update(context, settings);
	return context;
}

static void effect_3d_destroy(void *data)
{
	struct effect_3d *context = data;
	if (context->render) {
		obs_enter_graphics();
		gs_texrender_destroy(context->render);
		obs_leave_graphics();
	}
	bfree(context);
}

static bool effect_3d_mode_changed(obs_properties_t *props, obs_property_t *property, obs_data_t *settings)
{
	UNUSED_PARAMETER(property);
	const bool perspective = obs_data_get_int(settings, "mode") == MODE_PERSPECTIVE;
	obs_property_set_visible(obs_properties_get(props, "fov"), perspective);
	return true;
}

static bool effect_3d_direct_changed(obs_properties_t *props, obs_property_t *property, obs_data_t *settings)
{
	UNUSED_PARAMETER(property);
	// Dibujando directo no hay textura que recortar ni que reducir
	const bool direct = obs_data_get_bool(settings, "direct");
	obs_property_set_visible(obs_properties_get(props, "margin"), !direct);
	obs_property_set_visible(obs_properties_get(props, "supersample"), !direct);
	return true;
}

static obs_properties_t *effect_3d_properties(void *data)
{
	UNUSED_PARAMETER(data);
	obs_properties_t *ppts = obs_properties_create();

	obs_property_t *p =
		obs_properties_add_list(ppts, "mode", obs_module_text("Mode"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, obs_module_text("Perspective"), MODE_PERSPECTIVE);
	obs_property_list_add_int(p, obs_module_text("Orthographic"), MODE_ORTHOGRAPHIC);
	obs_property_set_modified_callback(p, effect_3d_mode_changed);

	p = obs_properties_add_float_slider(ppts, "fov", obs_module_text("FieldOfView"), 0.1, 180.0, 0.01);

	obs_properties_t *rot = obs_properties_create();

	p = obs_properties_add_float_slider(rot, "rot_x", obs_module_text("RotX"), -180.0, 180.0, 0.01);
	obs_property_float_set_suffix(p, "° Deg");

	p = obs_properties_add_float_slider(rot, "rot_y", obs_module_text("RotY"), -180.0, 180.0, 0.01);
	obs_property_float_set_suffix(p, "° Deg");

	p = obs_properties_add_float_slider(rot, "rot_z", obs_module_text("RotZ"), -180.0, 180.0, 0.01);
	obs_property_float_set_suffix(p, "° Deg");

	obs_properties_add_group(ppts, "rot", obs_module_text("Rotation"), OBS_GROUP_NORMAL, rot);

	obs_properties_t *pos = obs_properties_create();
	p = obs_properties_add_float(pos, "pos_x", obs_module_text("PosX"), -100000.0, 100000.0, 1.0);
	obs_property_float_set_suffix(p, " px");
	p = obs_properties_add_float(pos, "pos_y", obs_module_text("PosY"), -100000.0, 100000.0, 1.0);
	obs_property_float_set_suffix(p, " px");
	p = obs_properties_add_float(pos, "pos_z", obs_module_text("PosZ"), -100000.0, 100000.0, 1.0);
	obs_property_float_set_suffix(p, " px");

	obs_properties_add_group(ppts, "pos", obs_module_text("Position"), OBS_GROUP_NORMAL, pos);

	obs_properties_t *scale = obs_properties_create();

	p = obs_properties_add_float(scale, "scale_x", obs_module_text("ScaleX"), 0.0, 10000.0, 1.0f);
	obs_property_float_set_suffix(p, "%");
	p = obs_properties_add_float(scale, "scale_y", obs_module_text("ScaleY"), 0.0, 10000.0, 1.0f);
	obs_property_float_set_suffix(p, "%");

	obs_properties_add_group(ppts, "scale", obs_module_text("Scale"), OBS_GROUP_NORMAL, scale);

	obs_properties_t *shear = obs_properties_create();

	p = obs_properties_add_float_slider(shear, "shear_x", obs_module_text("ShearX"), -200.0, 200.0, 0.01);
	obs_property_float_set_suffix(p, "%");
	p = obs_properties_add_float_slider(shear, "shear_y", obs_module_text("ShearY"), -200.0, 200.0, 0.01);
	obs_property_float_set_suffix(p, "%");

	obs_properties_add_group(ppts, "shear", obs_module_text("Shear"), OBS_GROUP_NORMAL, shear);

	obs_properties_t *opts = obs_properties_create();

	p = obs_properties_add_bool(opts, "keep_size", obs_module_text("KeepSize"));
	obs_property_set_long_description(p, obs_module_text("KeepSize.Description"));

	p = obs_properties_add_float_slider(opts, "margin", obs_module_text("Margin"), 0.0, 100.0, 1.0);
	obs_property_float_set_suffix(p, "%");
	obs_property_set_long_description(p, obs_module_text("Margin.Description"));

	p = obs_properties_add_bool(opts, "supersample", obs_module_text("Supersample"));
	obs_property_set_long_description(p, obs_module_text("Supersample.Description"));

	obs_properties_add_bool(opts, "hide_back", obs_module_text("HideBack"));

	p = obs_properties_add_bool(opts, "direct", obs_module_text("Direct"));
	obs_property_set_long_description(p, obs_module_text("Direct.Description"));
	obs_property_set_modified_callback(p, effect_3d_direct_changed);

	obs_properties_add_group(ppts, "options", obs_module_text("Options"), OBS_GROUP_NORMAL, opts);

	obs_properties_add_text(ppts, "plugin_info",
				"3D Effect 2 (" PROJECT_VERSION ") by hjmeneses, based on "
				"<a href=\"https://obsproject.com/forum/resources/3d-effect.1692/\">3D Effect</a> by "
				"<a href=\"https://www.exeldro.com\">Exeldro</a>",
				OBS_TEXT_INFO);
	return ppts;
}

static const char *get_tech_name_and_multiplier(enum gs_color_space current_space, enum gs_color_space source_space,
						float *multiplier)
{
	const char *tech_name = "Draw";
	*multiplier = 1.f;

	switch (source_space) {
	case GS_CS_SRGB:
	case GS_CS_SRGB_16F:
		if (current_space == GS_CS_709_SCRGB) {
			tech_name = "DrawMultiply";
			*multiplier = obs_get_video_sdr_white_level() / 80.0f;
		}
		break;
	case GS_CS_709_EXTENDED:
		switch (current_space) {
		case GS_CS_SRGB:
		case GS_CS_SRGB_16F:
			tech_name = "DrawTonemap";
			break;
		case GS_CS_709_SCRGB:
			tech_name = "DrawMultiply";
			*multiplier = obs_get_video_sdr_white_level() / 80.0f;
			break;
		default:
			break;
		}
		break;
	case GS_CS_709_SCRGB:
		switch (current_space) {
		case GS_CS_SRGB:
		case GS_CS_SRGB_16F:
			tech_name = "DrawMultiplyTonemap";
			*multiplier = 80.0f / obs_get_video_sdr_white_level();
			break;
		case GS_CS_709_EXTENDED:
			tech_name = "DrawMultiply";
			*multiplier = 80.0f / obs_get_video_sdr_white_level();
			break;
		default:
			break;
		}
	}

	return tech_name;
}

// Pixeles que el margen agrega a cada lado de una dimensión
static uint32_t effect_3d_margin_px(const struct effect_3d *context, uint32_t size)
{
	if (context->direct)
		return 0;
	return (uint32_t)(size * context->margin / 100.0f + 0.5f);
}

static uint32_t effect_3d_get_width(void *data)
{
	struct effect_3d *context = data;
	obs_source_t *target = obs_filter_get_target(context->source);
	if (!target)
		return 0;
	const uint32_t w = obs_source_get_base_width(target);
	return w + 2 * effect_3d_margin_px(context, w);
}

static uint32_t effect_3d_get_height(void *data)
{
	struct effect_3d *context = data;
	obs_source_t *target = obs_filter_get_target(context->source);
	if (!target)
		return 0;
	const uint32_t h = obs_source_get_base_height(target);
	return h + 2 * effect_3d_margin_px(context, h);
}

static float lerpf(float a, float b, float f)
{
	return b * f + a * (1.0f - f);
}

// Mezcla dos transformaciones (f = 0: a, f = 1: b), para las transiciones de Move
static void effect_3d_transform_lerp(struct effect_3d_transform *dst, const struct effect_3d_transform *a,
				      const struct effect_3d_transform *b, float f)
{
	dst->fov = lerpf(a->fov, b->fov, f);
	vec3_set(&dst->position, lerpf(a->position.x, b->position.x, f), lerpf(a->position.y, b->position.y, f),
		 lerpf(a->position.z, b->position.z, f));
	vec3_set(&dst->rotation, lerpf(a->rotation.x, b->rotation.x, f), lerpf(a->rotation.y, b->rotation.y, f),
		 lerpf(a->rotation.z, b->rotation.z, f));
	vec2_set(&dst->scale, lerpf(a->scale.x, b->scale.x, f), lerpf(a->scale.y, b->scale.y, f));
	vec2_set(&dst->shear, lerpf(a->shear.x, b->shear.x, f), lerpf(a->shear.y, b->shear.y, f));
}

// Transformación neutra: lo que se ve sin el filtro (destino de "mover a cero")
static void effect_3d_transform_identity(struct effect_3d_transform *t)
{
	t->fov = 90.0f;
	vec3_zero(&t->position);
	vec3_zero(&t->rotation);
	vec2_set(&t->scale, 1.0f, 1.0f);
	vec2_zero(&t->shear);
}

// Proyección y matrices para dibujar la fuente (w x h) con la transformación t.
// kx, ky > 1 amplían la vista para el margen: la fuente ocupa el centro y lo que gira hacia
// afuera cabe en el resto.
static void effect_3d_set_projection(const struct effect_3d *context, const struct effect_3d_transform *t, float w,
				     float h, float kx, float ky)
{
	if (context->mode == MODE_ORTHOGRAPHIC) {
		gs_ortho(-kx, kx, -ky, ky, -(float)(1 << 22), (float)(1 << 22));
	} else {
		const float near_z = 1.0f / (float)(1 << 22);
		const float far_z = (float)(1 << 22);
		if (kx == 1.0f && ky == 1.0f) {
			gs_perspective(t->fov, w / h, near_z, far_z);
		} else {
			// lo mismo que gs_perspective, con el frustum ampliado
			const float ymax = near_z * tanf(RAD(t->fov) * 0.5f);
			const float xmax = ymax * (w / h);
			gs_frustum(-xmax * kx, xmax * kx, -ymax * ky, ymax * ky, near_z, far_z);
		}
	}
}

// Cámara y transformación de la fuente sobre la pila de matrices (espacio de vista)
static void effect_3d_apply_model(const struct effect_3d *context, const struct effect_3d_transform *t, float w, float h)
{
	if (context->mode != MODE_ORTHOGRAPHIC) {
		// Con la cámara a 1 la fuente sin girar solo llena su recuadro con FOV 90.
		// A 1/tan(fov/2) lo llena con cualquier FOV, como la perspectiva de CSS.
		float dist = 1.0f;
		if (context->keep_size)
			dist = 1.0f / tanf(RAD(fminf(t->fov, 179.0f)) * 0.5f);
		gs_matrix_translate3f(0.0f, 0.0f, -dist);
		gs_matrix_scale3f(w / h, 1.0f, 1.0f);
	}
	if (context->keep_size) {
		// Unidades reales: la posición va en px de la fuente (también Z) y no la cambia la escala
		gs_matrix_translate3f(t->position.x / w * 2.0f, t->position.y / h * 2.0f, t->position.z / h * 2.0f);
		gs_matrix_scale3f(t->scale.x, t->scale.y, 1.0f);
	} else {
		gs_matrix_scale3f(t->scale.x, t->scale.y, 1.0f);
		gs_matrix_translate3f(t->position.x / w * 2.0f, t->position.y / h * 2.0f, t->position.z / (w + h));
	}
	gs_matrix_scale3f(h / w, 1.0f, 1.0f);
	gs_matrix_rotaa4f(1.0f, 0.0f, 0.0f, RAD(t->rotation.x));
	gs_matrix_rotaa4f(0.0f, 1.0f, 0.0f, RAD(t->rotation.y));
	gs_matrix_rotaa4f(0.0f, 0.0f, 1.0f, RAD(t->rotation.z));
	gs_matrix_scale3f(w / h, 1.0f, 1.0f);
	struct matrix4 shear_matrix;
	matrix4_identity(&shear_matrix);
	shear_matrix.y.x = -t->shear.x;
	shear_matrix.x.y = t->shear.y;
	gs_matrix_mul(&shear_matrix);
	gs_matrix_translate3f(-1.0f, -1.0f, 0.0f);
	gs_matrix_scale3f(2.0f / w, 2.0f / h, 1.0f);
}

static void effect_3d_set_matrices(const struct effect_3d *context, const struct effect_3d_transform *t, float w, float h,
				   float kx, float ky)
{
	effect_3d_set_projection(context, t, w, h, kx, ky);
	effect_3d_apply_model(context, t, w, h);
}

// Matriz que lleva un pixel de la fuente (w x h) a su lugar en el recuadro de la propia fuente,
// con w de perspectiva y z = 0: se multiplica sobre la matriz de la escena y la GPU hace la
// división, así que lo que gira hacia afuera se dibuja fuera del recuadro (sin recorte) y la
// profundidad nunca se sale de rango. Es la misma idea del vertex shader de BFX.
static void effect_3d_direct_matrix(struct matrix4 *dst, const struct effect_3d *context,
				    const struct effect_3d_transform *t, float w, float h)
{
	struct matrix4 model, proj;
	gs_matrix_push();
	gs_matrix_identity();
	effect_3d_apply_model(context, t, w, h);
	gs_matrix_get(&model);
	gs_matrix_pop();

	if (context->mode == MODE_ORTHOGRAPHIC) {
		// ndc = vista; pixel = (ndc + 1) / 2 * tamaño
		vec4_set(&proj.x, w * 0.5f, 0.0f, 0.0f, 0.0f);
		vec4_set(&proj.y, 0.0f, h * 0.5f, 0.0f, 0.0f);
		vec4_set(&proj.z, 0.0f, 0.0f, 0.0f, 0.0f);
		vec4_set(&proj.t, w * 0.5f, h * 0.5f, 0.0f, 1.0f);
	} else {
		// ndc_x = X / (-Z tx), ndc_y = Y / (-Z ty), igual que el frustum de gs_perspective;
		// pixel·w' con w' = -Z
		const float ty = tanf(RAD(t->fov) * 0.5f);
		const float tx = ty * (w / h);
		vec4_set(&proj.x, w / (2.0f * tx), 0.0f, 0.0f, 0.0f);
		vec4_set(&proj.y, 0.0f, h / (2.0f * ty), 0.0f, 0.0f);
		vec4_set(&proj.z, -w * 0.5f, -h * 0.5f, 0.0f, -1.0f);
		vec4_set(&proj.t, 0.0f, 0.0f, 0.0f, 0.0f);
	}
	matrix4_mul(dst, &model, &proj);
}

// ¿La cara que se ve es la trasera? Se calcula con la normal de la fuente ya transformada:
// en reposo apunta hacia la cámara (+z) y el centro está delante (z < 0).
static bool effect_3d_facing_away(const struct effect_3d *context, float w, float h)
{
	struct matrix4 m;
	gs_matrix_get(&m);
	struct vec4 v, ex, ey, c;
	vec4_set(&v, w, 0.0f, 0.0f, 0.0f);
	vec4_transform(&ex, &v, &m);
	vec4_set(&v, 0.0f, h, 0.0f, 0.0f);
	vec4_transform(&ey, &v, &m);
	vec4_set(&v, w * 0.5f, h * 0.5f, 0.0f, 1.0f);
	vec4_transform(&c, &v, &m);

	struct vec3 a, b, n, center;
	vec3_set(&a, ex.x, ex.y, ex.z);
	vec3_set(&b, ey.x, ey.y, ey.z);
	vec3_cross(&n, &a, &b);
	if (context->mode == MODE_ORTHOGRAPHIC)
		return n.z < 0.0f;
	vec3_set(&center, c.x, c.y, c.z);
	return vec3_dot(&n, &center) > 0.0f;
}

void effect_3d_draw_frame(struct effect_3d *context, uint32_t w, uint32_t h)
{
	const enum gs_color_space current_space = gs_get_color_space();
	float multiplier;
	const char *technique = get_tech_name_and_multiplier(current_space, context->space, &multiplier);

	gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	gs_texture_t *tex = gs_texrender_get_texture(context->render);
	if (!tex)
		return;

	gs_blend_state_push();
	gs_blend_function_separate(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA, GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);

	const bool previous = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(true);

	gs_effect_set_texture_srgb(gs_effect_get_param_by_name(effect, "image"), tex);
	gs_effect_set_float(gs_effect_get_param_by_name(effect, "multiplier"), multiplier);

	// Con supersampling la textura mide el doble: el muestreo lineal promedia 2x2 pixeles
	while (gs_effect_loop(effect, technique))
		gs_draw_sprite(tex, 0, w, h);

	gs_enable_framebuffer_srgb(previous);
	gs_blend_state_pop();
}

// Dibuja la fuente tal cual en la textura (una vez por cuadro) y la pinta con la perspectiva
// directamente sobre la escena
static void effect_3d_render_direct(struct effect_3d *context, obs_source_t *target, obs_source_t *parent,
				    const struct effect_3d_transform *t)
{
	const uint32_t bw = obs_source_get_base_width(target);
	const uint32_t bh = obs_source_get_base_height(target);
	const float w = (float)bw, h = (float)bh;

	if (!context->processed_frame) {
		const enum gs_color_space preferred_spaces[] = {
			GS_CS_SRGB,
			GS_CS_SRGB_16F,
			GS_CS_709_EXTENDED,
		};
		const enum gs_color_space space =
			obs_source_get_color_space(target, OBS_COUNTOF(preferred_spaces), preferred_spaces);
		const enum gs_color_format format = gs_get_format_from_space(space);
		if (!context->render || gs_texrender_get_format(context->render) != format) {
			gs_texrender_destroy(context->render);
			context->render = gs_texrender_create(format, GS_ZS_NONE);
		} else {
			gs_texrender_reset(context->render);
		}
		if (gs_texrender_begin_with_color_space(context->render, bw, bh, space)) {
			gs_blend_state_push();
			gs_reset_blend_state();
			gs_enable_blending(false);
			gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
			struct vec4 clear_color;
			vec4_zero(&clear_color);
			gs_clear(GS_CLEAR_COLOR, &clear_color, 0.0f, 0);
			gs_ortho(0.0f, w, 0.0f, h, -100.0f, 100.0f);
			const uint32_t flags = obs_source_get_output_flags(target);
			if (target == parent && !(flags & OBS_SOURCE_CUSTOM_DRAW) && !(flags & OBS_SOURCE_ASYNC))
				obs_source_default_render(target);
			else
				obs_source_video_render(target);
			gs_blend_state_pop();
			gs_texrender_end(context->render);
			context->space = space;
		}
		context->processed_frame = true;
	}

	if (context->hide_back) {
		gs_matrix_push();
		gs_matrix_identity();
		effect_3d_apply_model(context, t, w, h);
		const bool away = effect_3d_facing_away(context, w, h);
		gs_matrix_pop();
		if (away)
			return;
	}

	struct matrix4 m;
	effect_3d_direct_matrix(&m, context, t, w, h);
	gs_matrix_push();
	gs_matrix_mul(&m);
	effect_3d_draw_frame(context, bw, bh);
	gs_matrix_pop();
}

void effect_3d_video_render(void *data, gs_effect_t *eff)
{
	UNUSED_PARAMETER(eff);
	struct effect_3d *context = data;

	obs_source_t *target = obs_filter_get_target(context->source);
	obs_source_t *parent = obs_filter_get_parent(context->source);
	uint32_t base_width = obs_source_get_base_width(target);
	uint32_t base_height = obs_source_get_base_height(target);
	if (!base_width || !base_height || !target || !parent) {
		obs_source_skip_video_filter(context->source);
		return;
	}

	// Tamaño de salida con el margen
	const uint32_t out_width = base_width + 2 * effect_3d_margin_px(context, base_width);
	const uint32_t out_height = base_height + 2 * effect_3d_margin_px(context, base_height);

	float f = 0.0f;
	obs_source_t *filter_to = NULL;
	if (move_get_transition_filter)
		f = move_get_transition_filter(context->source, &filter_to);

	// Transformación de este cuadro: la del filtro o, durante una transición de Move,
	// la mezcla con el otro filtro (o con la neutra al ir a cero)
	struct effect_3d_transform t = context->transform;
	if (f > 0.0f && filter_to != context->source) {
		struct effect_3d_transform to;
		if (filter_to) {
			struct effect_3d *context2 = obs_obj_get_data(filter_to);
			to = context2->transform;
		} else {
			effect_3d_transform_identity(&to);
		}
		effect_3d_transform_lerp(&t, &context->transform, &to, f);
	}

	if (context->direct) {
		effect_3d_render_direct(context, target, parent, &t);
		return;
	}

	if (f == 0.0f && context->processed_frame) {
		effect_3d_draw_frame(context, out_width, out_height);
		return;
	}

	const enum gs_color_space preferred_spaces[] = {
		GS_CS_SRGB,
		GS_CS_SRGB_16F,
		GS_CS_709_EXTENDED,
	};
	const enum gs_color_space space = obs_source_get_color_space(target, OBS_COUNTOF(preferred_spaces), preferred_spaces);
	const enum gs_color_format format = gs_get_format_from_space(space);
	if (!context->render || gs_texrender_get_format(context->render) != format) {
		gs_texrender_destroy(context->render);
		context->render = gs_texrender_create(format, GS_ZS_NONE);
	} else {
		gs_texrender_reset(context->render);
	}

	const uint32_t ss = context->supersample ? 2 : 1;
	if (gs_texrender_begin_with_color_space(context->render, out_width * ss, out_height * ss, space)) {
		gs_viewport_push();
		gs_matrix_push();
		gs_blend_state_push();
		gs_reset_blend_state();
		gs_enable_blending(false);
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);

		const float w = (float)base_width;
		const float h = (float)base_height;
		struct vec4 clear_color;

		uint32_t parent_flags = obs_source_get_output_flags(target);
		bool custom_draw = (parent_flags & OBS_SOURCE_CUSTOM_DRAW) != 0;
		bool async = (parent_flags & OBS_SOURCE_ASYNC) != 0;
		vec4_zero(&clear_color);
		gs_clear(GS_CLEAR_COLOR, &clear_color, 0.0f, 0);

		effect_3d_set_matrices(context, &t, w, h, (float)out_width / w, (float)out_height / h);

		if (!context->hide_back || !effect_3d_facing_away(context, w, h)) {
			if (target == parent && !custom_draw && !async)
				obs_source_default_render(target);
			else
				obs_source_video_render(target);
		}
		gs_blend_state_pop();
		gs_matrix_pop();
		gs_viewport_pop();
		gs_texrender_end(context->render);
		context->space = space;
	}
	if (f == 0.0f)
		context->processed_frame = true;
	effect_3d_draw_frame(context, out_width, out_height);
}

void effect_3d_video_tick(void *data, float seconds)
{
	UNUSED_PARAMETER(seconds);
	struct effect_3d *context = data;
	context->processed_frame = false;
}

void effect_3d_defaults(obs_data_t *settings)
{
	obs_data_set_default_double(settings, "fov", 90.0);
	obs_data_set_default_double(settings, "scale_x", 100.0);
	obs_data_set_default_double(settings, "scale_y", 100.0);
	obs_data_set_default_double(settings, "shear_x", 0.0);
	obs_data_set_default_double(settings, "shear_y", 0.0);
	// Las opciones nuevas van apagadas para que las escenas existentes se vean igual
	obs_data_set_default_bool(settings, "keep_size", false);
	obs_data_set_default_double(settings, "margin", 0.0);
	obs_data_set_default_bool(settings, "supersample", false);
	obs_data_set_default_bool(settings, "hide_back", false);
}

OBS_DECLARE_MODULE()
OBS_MODULE_AUTHOR("hjmeneses, based on 3D Effect by Exeldro");
OBS_MODULE_USE_DEFAULT_LOCALE("3d-effect-2", "en-US")
MODULE_EXPORT const char *obs_module_description(void)
{
	return obs_module_text("Description");
}

MODULE_EXPORT const char *obs_module_name(void)
{
	return obs_module_text("3DEffect");
}

struct obs_source_info effect_3d_info = {
	.id = "3d_effect_2_filter", // distinto del original: los dos conviven
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_SRGB,
	.get_name = effect_3d_get_name,
	.create = effect_3d_create,
	.destroy = effect_3d_destroy,
	.get_properties = effect_3d_properties,
	.get_defaults = effect_3d_defaults,
	.video_render = effect_3d_video_render,
	.video_tick = effect_3d_video_tick,
	.update = effect_3d_update,
	.load = effect_3d_update,
	.get_width = effect_3d_get_width,
	.get_height = effect_3d_get_height,
};

bool obs_module_load(void)
{
	blog(LOG_INFO, "[3D Effect 2] loaded version %s", PROJECT_VERSION);
	obs_register_source(&effect_3d_info);
	return true;
}

void obs_module_post_load()
{
	if (obs_get_module("move-transition") == NULL)
		return;
	proc_handler_t *ph = obs_get_proc_handler();
	struct calldata cd;
	calldata_init(&cd);
	calldata_set_string(&cd, "filter_id", effect_3d_info.id);
	if (proc_handler_call(ph, "move_get_transition_filter_function", &cd)) {
		move_get_transition_filter = calldata_ptr(&cd, "callback");
	}
	calldata_free(&cd);
}
