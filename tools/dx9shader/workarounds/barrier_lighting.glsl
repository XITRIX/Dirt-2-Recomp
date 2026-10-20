// Full material evaluation; samplers, constants and varyings use the translated shader interface.
float dirt2_barrier_rsq(float x) { return x == 0.0 ? 3.402823466e38 : inversesqrt(abs(x)); }
void main() {
    vec3 normal = ps_v1.xyz * dirt2_barrier_rsq(dot(ps_v1.xyz,ps_v1.xyz));
    float hemisphere = clamp((normal.y + 0.30000001192092896) * 0.7692307829856873,0.0,1.0);
    float sunDot = dot(normal,ps_uniforms_1._m0[1].xyz);
    vec3 ambient = hemisphere * ps_uniforms_1._m0[3].rgb
                 + clamp((0.30000001192092896-sunDot)*0.7692307829856873,0.0,1.0)*ps_uniforms_1._m0[5].rgb + ps_uniforms_1._m0[9].rgb;
    float visibility = 0.5-ps_uniforms_1._m0[12].x >= 0.0 ? ps_v1.w : 1.0;
    float inverseDistance = dirt2_barrier_rsq(dot(ps_v3.xyz,ps_v3.xyz));
    vec3 viewDirection = ps_v3.xyz * inverseDistance;
    float distance = inverseDistance == 0.0 ? 3.402823466e38 : 1.0/inverseDistance;
    float attenuation = clamp(distance*distance*ps_uniforms_1._m0[10].x+ps_uniforms_1._m0[10].y,0.0,1.0);
    vec2 screenUV = (gl_FragCoord.xy-vec2(0.5))*ps_uniforms_1._m0[0].zw;
    vec4 lighting = texture(ps_s2,screenUV);
    float shadow = texture(ps_s3,screenUV).r;
    float lightVisibility = mix(lighting.r,visibility,attenuation);
    vec3 sun = clamp(sunDot,0.0,1.0)*ps_uniforms_1._m0[2].rgb*(1.0+ps_uniforms_1._m0[2].w*(lightVisibility-1.0))
             + lighting.gba*ps_uniforms_1._m0[11].w;
    vec3 irradiance = ambient*ps_uniforms_1._m0[4].w+sun;
    vec3 reflection = viewDirection-2.0*dot(viewDirection,normal)*normal;
    float highlight = clamp(dot(reflection,ps_uniforms_1._m0[7].xyz),0.0,1.0);
    vec4 material = texture(ps_s1,ps_v2.xy);
    float exponent = mix(ps_uniforms_1._m0[6].x,ps_uniforms_1._m0[6].y,material.g);
    vec3 specular = pow(abs(highlight),exponent)*ps_uniforms_1._m0[8].rgb*(material.r*ps_uniforms_1._m0[6].z)*lightVisibility;
    vec4 base = texture(ps_s0,ps_v2.xy);
    vec3 lit = (base.rgb*irradiance+specular)*shadow;
    vec3 fogged = lit*ps_v0.w+ps_v0.rgb;
    ps_oC0 = vec4(fogged*ps_uniforms_1._m0[14].rgb/(fogged*ps_uniforms_1._m0[13].rgb+vec3(1.0)),base.a);
}
