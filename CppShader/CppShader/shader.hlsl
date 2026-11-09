struct VSInput  { float3 pos : POSITION; };
struct VSOutput { float4 pos : SV_POSITION; };

cbuffer CBTransform : register(b0) // C++의 슬롯 0과 대응
{
    row_major float4x4 wvp;
};

VSOutput VSMain(VSInput input)
{
    VSOutput output;
    output.pos = mul(float4(input.pos, 1.0f), wvp);
    return output;
}

float4 PSMain(VSOutput input) : SV_Target
{
    return float4(1.0f, 0.0f, 0.0f, 1.0f);
}