//float warp = 0.75; // simulate curvature of CRT monitor
//float scan = 0.75; // simulate darkness between scanlines
//
//void mainImage(out vec4 fragColor,in vec2 fragCoord)
//{
//    // squared distance from center
//    vec2 uv = fragCoord/iResolution.xy;
//    vec2 dc = abs(0.5-uv);
//    dc *= dc;
//    
//    // warp the fragment coordinates
//    uv.x -= 0.5; uv.x *= 1.0+(dc.y*(0.3*warp)); uv.x += 0.5;
//    uv.y -= 0.5; uv.y *= 1.0+(dc.x*(0.4*warp)); uv.y += 0.5;
//
//    // sample inside boundaries, otherwise set to black
//    if (uv.y > 1.0 || uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0)
//        fragColor = vec4(0.0,0.0,0.0,1.0);
//    else
//    	{
//        // determine if we are drawing in a scanline
//        float apply = abs(sin(fragCoord.y)*0.5*scan);
//        // sample the texture
//    	fragColor = vec4(mix(texture(iChannel0,uv).rgb,vec3(0.0),apply),1.0);
//        }
//}

// Common Shader Code
#include "shader_common.h"

// Fullscreen Vertex Shader
#include "fullscreen_vs.glslh"

@fs fs

layout(binding=0) uniform texture2D tex;

layout(binding=0) uniform sampler smp;

layout(binding=0) uniform fs_params {
	float warp;
	float scan; 
	int crt_enabled;
};

in vec2 uv;

out vec4 frag_color;

void main()
{
	if (crt_enabled == 0)
	{
		frag_color = texture(sampler2D(tex, smp), uv);
		return;
	}

	vec2 uv = uv;

    // squared distance from center
    vec2 dc = abs(0.5-uv);
    dc *= dc;
    
    // warp the fragment coordinates
    uv.x -= 0.5; uv.x *= 1.0+(dc.y*(0.3*warp)); uv.x += 0.5;
    uv.y -= 0.5; uv.y *= 1.0+(dc.x*(0.4*warp)); uv.y += 0.5;

    // sample inside boundaries, otherwise set to black
    if (uv.y > 1.0 || uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0)
	{
        frag_color = vec4(0.0,0.0,0.0,1.0);
	}
    else
	{
		//texture(sampler2D(tex, smp), uv);

		// determine if we are drawing in a scanline
		float apply = abs(sin(gl_FragCoord.y)*0.5*scan);
		// sample the texture
		frag_color = vec4(mix(texture(sampler2D(tex, smp), uv).rgb,vec3(0.0),apply),1.0);
	}
}

@end

@program crt vs fs
