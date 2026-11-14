struct VSInput  { float3 pos : POSITION; float2 uv : TEXCOORD0; };
struct VSOutput { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

Texture2D    gTex  : register(t0);   // 새 변수: SRV가 연결될 슬롯 t0
SamplerState gSamp : register(s0);   // 새 변수: 샘플러가 연결될 슬롯 s0

cbuffer CBTransform : register(b0) // C++의 슬롯 0과 대응
{
    row_major float4x4 wvp;
};

VSOutput VSMain(VSInput input)
{
    VSOutput output;
    output.pos = mul(float4(input.pos, 1.0f), wvp);
    output.uv = input.uv;
    return output;
}

float4 PSMain(VSOutput input) : SV_Target
{
    return gTex.Sample(gSamp, input.uv);
}