//
// RT64
//

struct Constants {
    float4 colorAdd;
    uint textureIndex;
};

[[vk::push_constant]] ConstantBuffer<Constants> gConstants : register(b0);

SamplerState gSampler : register(s1);
Texture2D<float4> gTexture[] : register(t2);

float4 PSMain(in float4 pos : SV_Position, in float2 uv : TEXCOORD) : SV_TARGET {
    float4 result = float4(gTexture[NonUniformResourceIndex(gConstants.textureIndex)].SampleLevel(gSampler, uv, 0).rgb, 1.0f);
    result += gConstants.colorAdd;
    return result;
}