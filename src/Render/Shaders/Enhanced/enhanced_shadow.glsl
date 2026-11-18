// The enhanced mode's shadow.glsl (not retail): the lit shaders' enhanced
// variants (Enhanced/*.patch, built by cmake/ShaderVariant.cmake) include
// this in place of shadow.glsl. The block starts with shadow.glsl's
// members (ShadowUniforms) and adds:
//   - cascaded shadow maps: four cascades in a 2 x 2 atlas per caster set
//     (the dynamic objects' at SHADOW_MAP_BINDING, the static meshes' and
//     the terrain's at the next), picked by the view distance, filtered
//     with a 3 x 3 grid of comparison taps (PCF); cascadeParams.x 0 keeps
//     shadow.glsl's single map;
//   - the ambient occlusion (enhancedAO, ENHANCED_AO_BINDING): the GTAO
//     pass's result at this pixel, which the variants multiply into their
//     ambient term only;
//   - a Titan's hull beyond the cascades (enhanced.titanShadows): a tile
//     fitted to each Titan in a third row of the dynamic atlas;
//   - enhanced.staticShadows: every receiver takes the static atlas too;
//   - enhanced.softShadows: softShadow in the two nearest cascades;
//   - enhancedSimpleShadow: the light manager's SimpleShadowmap factor
//     gives way to the cascades where they reach.
// The includer defines SHADOW_UNIFORM_BINDING, SHADOW_MAP_BINDING,
// optionally HEMI_MAP_BINDING (as for shadow.glsl) and ENHANCED_AO_BINDING.

layout(set = 3, binding = SHADOW_UNIFORM_BINDING) uniform Shadows {
	mat4 shadowMatrix;  // world -> the maps' uv (xy) and depth (z)
	vec4 shadowParams;  // x: 1 with shadows, y: a texel in uv, z: depth bias, w: 1 with the static map
	vec4 hemiConstants; // xy: the map's corner (world x, z), z: its size (0: no hemi map), w: lerp bias
	vec4 hemiSky;       // rgb: HemiMapSkyColor, w: 1 to take the sun's occlusion from it
	vec4 shadowExtra;   // x: shadowIntensityBias, y: the fade's start (share of the region), z: 1 for undergrowth
	// The enhanced members (EnhancedShadowUniforms).
	mat4 cascadeMatrix[4]; // world -> the cascade's uv in a 2 x 2 atlas (xy) and depth (z)
	vec4 cascadeFar;       // the view distance each cascade ends at
	vec4 cascadeBias;      // each cascade's depth bias
	vec4 cascadeParams;    // x: cascades (0: shadow.glsl's map), y: a texel in 2 x 2 atlas uv, z: 1 with the static atlas, w: 1 when every receiver takes it (enhanced.staticShadows)
	vec4 viewEye;          // xyz: the camera
	vec4 viewForward;      // xyz: its view axis
	vec4 aoParams;         // xy: 1 / the target's size, z: 1 to apply the AO, w: its strength
	mat4 titanMatrix[2];   // world -> a Titan's tile uv in the dynamic atlas (xy) and depth (z)
	vec4 titanParams;      // x: Titans mapped, y: a texel in 2 x 2 atlas uv, z: depth bias
	vec4 cascadeTexel;     // metres a texel, each cascade
	vec4 softParams;       // x: the light's depth a metre, y: penumbra radius a metre of caster distance (0: plain PCF), z: TAA's frame
};

layout(set = 2, binding = SHADOW_MAP_BINDING) uniform sampler2DShadow dynamicShadowMap;
layout(set = 2, binding = SHADOW_MAP_BINDING + 1) uniform sampler2DShadow staticShadowMap;

// The dynamic atlas is 2 x 3 tiles (the cascades' 2 x 2, then the Titans'
// row), the static one 2 x 2: a 2 x 2 uv's place in the dynamic one.
const vec2 kDynamicScale = vec2(1.0, 2.0 / 3.0);

float sampleShadow4(sampler2DShadow map, vec3 p, float texel)
{
	float h = texel * 0.5;
	return 0.25 * (texture(map, vec3(p.xy + vec2(-h, -h), p.z)) + texture(map, vec3(p.xy + vec2(h, -h), p.z)) +
		texture(map, vec3(p.xy + vec2(-h, h), p.z)) + texture(map, vec3(p.xy + vec2(h, h), p.z)));
}

// Nine filtered comparison taps a texel apart: a 4 x 4 texel tent.
float samplePcf(sampler2DShadow map, vec3 p, vec2 texel)
{
	float sum = 0.0;
	for (int y = -1; y <= 1; ++y) {
		for (int x = -1; x <= 1; ++x) {
			sum += texture(map, vec3(p.xy + vec2(float(x), float(y)) * texel, p.z));
		}
	}
	return sum / 9.0;
}

// enhanced.softShadows: sharp where the caster touches the receiver, wider
// with the distance between them (PCSS's idea). The maps are bound with
// comparison samplers only (no slot is left for their depths), so the
// caster's distance comes from comparisons at depths stepped towards the
// light: the farthest step that still finds an occluder. `texelMetres` is a
// texel's size on the ground.
float softShadow(sampler2DShadow map, vec3 p, vec2 texel, float texelMetres)
{
	// Interleaved gradient noise turns the taps per pixel (per frame under TAA).
	vec2 pixel = gl_FragCoord.xy + 5.588238 * softParams.z;
	float turn = 6.2831853 * fract(52.9829189 * fract(dot(pixel, vec2(0.06711056, 0.00583715))));
	vec2 axis = vec2(cos(turn), sin(turn));
	vec2 ring[4] = vec2[4](axis, vec2(-axis.y, axis.x), -axis, vec2(axis.y, -axis.x));
	float open = texture(map, p);
	for (int k = 0; k < 4; ++k) {
		open += texture(map, vec3(p.xy + ring[k] * 3.0 * texel, p.z));
	}
	if (open > 4.999) {
		return 1.0; // nothing near stands in the light
	}
	float distance = 0.0;
	for (int s = 0; s < 4; ++s) {
		float step = 0.1 * pow(4.0, float(s)); // 0.1, 0.4, 1.6, 6.4 m
		float z = p.z - step * softParams.x;
		float lit = texture(map, vec3(p.xy, z));
		for (int k = 0; k < 4; ++k) {
			lit += texture(map, vec3(p.xy + ring[k] * 3.0 * texel, z));
		}
		if (lit > 4.999) {
			break;
		}
		distance = step;
	}
	float radius = clamp(distance * softParams.y / max(texelMetres, 1e-4), 1.0, 8.0); // texels
	// A wider disc lies over more of a sloping receiver: its depth of bias with it.
	float z = p.z - radius * texelMetres * softParams.x;
	float sum = 0.0;
	for (int k = 0; k < 12; ++k) {
		float r = sqrt((float(k) + 0.5) / 12.0) * radius;
		float a = float(k) * 2.3999632 + turn;
		sum += texture(map, vec3(p.xy + vec2(cos(a), sin(a)) * r * texel, z));
	}
	return sum / 12.0;
}

// The sun past the cascades: what the Titans' hulls leave of it
// (enhanced.titanShadows: their tiles in the dynamic atlas's third row).
float farShadow(vec3 worldPosition)
{
	float lit = 1.0;
	int count = int(titanParams.x + 0.5);
	vec2 texel = titanParams.y * kDynamicScale;
	vec2 margin = texel * 2.0;
	for (int i = 0; i < 2; ++i) {
		if (i >= count) {
			continue;
		}
		vec4 p = titanMatrix[i] * vec4(worldPosition, 1.0);
		vec2 local = p.xy - vec2(float(i) * 0.5, 2.0 / 3.0);
		if (all(greaterThan(local, margin)) && all(lessThan(local, vec2(0.5, 1.0 / 3.0) - margin)) && p.z > 0.0 && p.z < 1.0) {
			lit = min(lit, samplePcf(dynamicShadowMap, vec3(p.xy, p.z - titanParams.z), texel));
		}
	}
	return clamp(lit + shadowExtra.x, 0.0, 1.0);
}

float viewDistance(vec3 worldPosition)
{
	return dot(worldPosition - viewEye.xyz, viewForward.xyz);
}

// The cascade holding the point (-1: none), and its 2 x 2 atlas position.
int pickCascade(vec3 worldPosition, out vec3 q)
{
	float d = viewDistance(worldPosition);
	int count = int(cascadeParams.x + 0.5);
	// The filters' reach from the quadrant's edge (softShadow's disc and search).
	float margin = cascadeParams.y * 11.0;
	for (int i = 0; i < 4; ++i) {
		if (i >= count || d > cascadeFar[i]) {
			continue;
		}
		vec4 p = cascadeMatrix[i] * vec4(worldPosition, 1.0);
		vec2 quadrant = vec2(float(i & 1), float(i >> 1)) * 0.5;
		vec2 local = p.xy - quadrant;
		if (all(greaterThan(local, vec2(margin))) && all(lessThan(local, vec2(0.5 - margin))) && p.z > 0.0 && p.z < 1.0) {
			q = vec3(p.xy, p.z - cascadeBias[i]);
			return i;
		}
	}
	return -1;
}

// 1 lit, 0 in shadow.
float shadowFactor(vec3 worldPosition, bool withStatic)
{
	if (cascadeParams.x > 0.5) {
		vec3 q;
		int cascade = pickCascade(worldPosition, q);
		if (cascade < 0) {
			return farShadow(worldPosition);
		}
		// The two nearest cascades, where a penumbra's width shows.
		bool soft = softParams.y > 0.0 && cascade < 2;
		vec2 texel = vec2(cascadeParams.y);
		vec3 qd = vec3(q.xy * kDynamicScale, q.z);
		float lit = soft ? softShadow(dynamicShadowMap, qd, texel * kDynamicScale, cascadeTexel[cascade])
						 : samplePcf(dynamicShadowMap, qd, texel * kDynamicScale);
		if ((withStatic || cascadeParams.w > 0.5) && cascadeParams.z > 0.5) {
			lit = min(lit, soft ? softShadow(staticShadowMap, q, texel, cascadeTexel[cascade]) : samplePcf(staticShadowMap, q, texel));
		}
		lit = clamp(lit + shadowExtra.x, 0.0, 1.0);
		// The last cascade fades out over its last tenth.
		int last = int(cascadeParams.x + 0.5) - 1;
		float fade = cascade == last ? clamp((viewDistance(worldPosition) / cascadeFar[last] - 0.9) * 10.0, 0.0, 1.0) : 0.0;
		return fade > 0.0 ? mix(lit, farShadow(worldPosition), fade) : lit;
	}
	if (shadowParams.x < 0.5) {
		return 1.0;
	}
	vec4 p = shadowMatrix * vec4(worldPosition, 1.0);
	vec2 edge = abs(p.xy * 2.0 - 1.0);
	float reach = max(edge.x, edge.y);
	if (reach >= 1.0 || p.z >= 1.0 || p.z <= 0.0) {
		return 1.0;
	}
	vec3 q = vec3(p.xy, p.z - shadowParams.z);
	float lit = sampleShadow4(dynamicShadowMap, q, shadowParams.y);
	if (withStatic && shadowParams.w > 0.5) {
		lit = min(lit, sampleShadow4(staticShadowMap, q, shadowParams.y));
	}
	lit = clamp(lit + shadowExtra.x, 0.0, 1.0);
	float fade = clamp((reach - shadowExtra.y) / max(1.0 - shadowExtra.y, 1e-3), 0.0, 1.0);
	return mix(lit, 1.0, fade);
}

// The SimpleShadowmap factor (mesh.frag's `simple`): the cascades, which
// hold the static meshes and the terrain, take over where they reach.
float enhancedSimpleShadow(float simple, vec3 worldPosition)
{
	if (cascadeParams.x < 0.5 || cascadeParams.z < 0.5) {
		return simple;
	}
	float reach = viewDistance(worldPosition) / cascadeFar[int(cascadeParams.x + 0.5) - 1];
	return mix(1.0, simple, clamp((reach - 0.9) * 10.0, 0.0, 1.0));
}

#ifdef ENHANCED_AO_BINDING
layout(set = 2, binding = ENHANCED_AO_BINDING) uniform sampler2D enhancedAOMap;

float enhancedAO()
{
	if (aoParams.z < 0.5) {
		return 1.0;
	}
	return mix(1.0, texture(enhancedAOMap, gl_FragCoord.xy * aoParams.xy).r, aoParams.w);
}
#endif

#ifdef HEMI_MAP_BINDING
layout(set = 2, binding = HEMI_MAP_BINDING) uniform sampler2D hemiMap;

// As shadow.glsl's.
vec4 hemiLight(vec3 worldPosition, vec3 normal, float objectY, vec3 sky, vec4 object)
{
	if (hemiConstants.z <= 0.0) {
		return vec4(sky, 1.0);
	}
	vec2 uv = ((worldPosition + vec3(hemiConstants.z * 0.5) + normal).xz - hemiConstants.xy) / hemiConstants.z;
	uv.y = 1.0 - uv.y;
	vec4 ground = texture(hemiMap, uv);
	// Inside a Titan (hemiSky.w 2): RendDX9's 0x65 grey ground (shadow.glsl).
	if (hemiSky.w > 1.5) ground = vec4(101.0 / 255.0);
	float localHeight = (worldPosition.y - objectY) * 0.5;
	float overTerrain = object.w;
	float offset = clamp(localHeight * 2.0 - 1.0 + overTerrain, -2.0 * (1.0 - overTerrain), 0.8);
	float lerpFactor = clamp((normal.y + offset) * 0.5 + 0.5, 0.0, 0.9);
	return vec4(mix(ground.rgb, hemiSky.rgb, lerpFactor) * object.rgb, hemiSky.w > 0.5 ? ground.a : 1.0);
}
#endif
