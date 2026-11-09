struct VSInput  { float3 pos : POSITION; };
struct VSOutput { float4 pos : SV_POSITION; };

VSOutput VSMain(VSInput input)
{
    VSOutput output;
    output.pos = float4(input.pos, 1.0f);
    return output;
}

float4 PSMain(VSOutput input) : SV_Target
{
    return float4(1.0f, 0.0f, 0.0f, 1.0f);
}