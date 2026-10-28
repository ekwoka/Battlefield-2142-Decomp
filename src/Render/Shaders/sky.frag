#version 450
// The sky dome (SkyDome.fx): the sky texture on the first coordinates and,
// with cloud layers, their textures on the second, scrolled by TEXOFFSET
// (TEXOFFSET2), faded out from CLOUDSFADEOUTDIST.x over .y of the dome's
// radius and below the horizon:
//   one layer  (psSkyDome):           lerp(sky, cloud, cloud.a)
//   two layers (psSkyDomeDualClouds): c = cloud * f.x + cloud2 * f.y; lerp(sky, c, c.a)
layout(location = 1) in vec2 inTexCoord;
layout(location = 3) in vec2 inCloudCoord; // mesh.vert's second set (the lightmap slot)
layout(location = 7) in vec3 inWorldPosition;

layout(set = 2, binding = 0) uniform sampler2D skyTexture;
layout(set = 2, binding = 1) uniform sampler2D cloudTexture;
layout(set = 2, binding = 2) uniform sampler2D cloudTexture2;

layout(set = 3, binding = 0) uniform Sky {
	vec4 domeCenter;   // xyz: the dome's centre (the camera), w: the layers (0, 1, 2)
	vec4 cloudOffsets; // xy: TEXOFFSET, zw: TEXOFFSET2
	vec4 cloudFade;    // x: the fade's start, y: its length (dome units), zw: CLOUDLERPFACTORS
	vec4 domeScale;    // x: 1 / the dome's scale (world to dome units)
};

layout(location = 0) out vec4 outColor;

void main()
{
	vec4 sky = texture(skyTexture, inTexCoord);
	int layers = int(domeCenter.w + 0.5);
	if (layers == 0) {
		outColor = vec4(sky.rgb, 1.0);
		return;
	}
	vec3 local = (inWorldPosition - domeCenter.xyz) * domeScale.x;
	float fade = 1.0 - clamp((length(local) - cloudFade.x) / max(cloudFade.y, 1e-3), 0.0, 1.0);
	fade *= local.y > 0.0 ? 1.0 : 0.0;
	vec4 cloud = texture(cloudTexture, inCloudCoord + cloudOffsets.xy);
	if (layers == 1) {
		cloud *= fade;
		outColor = vec4(mix(sky.rgb, cloud.rgb, cloud.a), 1.0);
		return;
	}
	vec4 cloud2 = texture(cloudTexture2, inCloudCoord + cloudOffsets.zw);
	vec4 c = (cloud * cloudFade.z + cloud2 * cloudFade.w) * fade;
	outColor = vec4(mix(sky.rgb, c.rgb, c.a), 1.0);
}
