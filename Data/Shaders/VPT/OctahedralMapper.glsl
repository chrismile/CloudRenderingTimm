-- Compute

#version 460

#extension GL_EXT_scalar_block_layout : enable

layout(local_size_x = BLOCK_SIZE, local_size_y = BLOCK_SIZE) in;

layout(binding = 0) uniform sampler2D environmentMapTexture;

layout(binding = 1, r32f) uniform writeonly image2D outputImage;

const float PI = 3.14159265359;

vec3 octahedralUVToWorld(vec2 uv) {
    uv = uv * 2. - 1.;
    vec2 sgn = sign(uv);
    uv = abs(uv);

    float r = 1. - abs(1. - uv.x - uv.y);
    float phi = .25 * PI * ( (uv.y - uv.x) / r + 1. );
    if (r == 0.){
        phi = 0.;
    }

    float x = sgn.x * cos(phi) * r * sqrt(2 - r * r);
    float y = sgn.y * sin(phi) * r * sqrt(2 - r * r);
    float z = sign(1. - uv.x - uv.y) * (1. - r * r);
    return vec3(x, y, z);
}


vec3 sampleSkybox(in vec3 dir) {
    // Sample from equirectangular projection.
    vec2 texcoord = vec2(atan(dir.z, dir.x) / (2. * PI) + 0.5, -asin(dir.y) / PI + 0.5);
    vec3 textureColor = texture(environmentMapTexture, texcoord).rgb;
    //textureColor = vec3(texcoord, 0.);
    return textureColor;
}

void main() {
    ivec2 writePos = ivec2(gl_GlobalInvocationID.xy);
    ivec2 outputImageSize = imageSize(outputImage);


    if (writePos.x < outputImageSize.x && writePos.y < outputImageSize.y) {
        float maxBrightness = -1.;
        for (float x = .1; x < 1.; x += .2)  {
            for (float y = .1; y < 1.; y += .2){
                vec2 uv = (vec2(gl_GlobalInvocationID.xy) + vec2(x, y)) / vec2(outputImageSize);

                vec3 dir = octahedralUVToWorld(uv);
                vec3 skybox = sampleSkybox(dir);
                skybox = min(skybox, vec3(100000, 100000, 100000));
                float brightness = length(skybox);
                maxBrightness = max(maxBrightness, brightness);
            }
        }

        imageStore(outputImage, writePos, vec4(maxBrightness,0,0,0));
    }
}