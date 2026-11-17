struct VSInput  { float3 pos : POSITION; float2 uv : TEXCOORD0; float3 normal : NORMAL; };
struct VSOutput { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; float3 normal : TEXCOORD1; float3 worldPos : TEXCOORD2;};

Texture2D    gTex  : register(t0);   // 새 변수: SRV가 연결될 슬롯 t0
SamplerState gSamp : register(s0);   // 새 변수: 샘플러가 연결될 슬롯 s0

cbuffer CBTransform : register(b0) // C++의 슬롯 0과 대응
{
    row_major float4x4 wvp;
    row_major float4x4 world;   // C++ 쪽 순서와 같아야 해요
    float3 cameraPos;   // C++ 필드와 같은 순서
    float  pad;
};

VSOutput VSMain(VSInput input)
{
    VSOutput output;
    output.pos = mul(float4(input.pos, 1.0f), wvp);
    output.uv = input.uv;
    output.normal = mul(input.normal, world).xyz;
    output.worldPos = mul(float4(input.pos, 1.0f), world).xyz;
    return output;
}

float4 PSMain(VSOutput input) : SV_Target
{
    float3 N = normalize(input.normal);                      // 월드 N (보간 뒤 길이가 1이 아닐 수 있어서 다시 정규화)
    float3 L = normalize(-float3(-1.0f, -1.0f, 0.0f));       // 빛이 가는 방향 (-1,-1,0)의 반대 = 표면에서 광원 쪽
    float diffuse = max(dot(N, L), 0.0f);
    
    float3 V = normalize(cameraPos - input.worldPos);   // 표면 점에서 카메라 쪽
    float3 R = reflect(-L, N);                          // 3-1의 R
    float shininess = 4.0f;
    float spec = 0.0f;
    if (diffuse > 0.0f)                                 // 빛이 닿는 면만
        spec = pow(max(dot(R, V), 0.0f), shininess);
    
    float ambient = 0.1f;                                    // 빛이 안 닿는 면도 완전히 까맣지 않게 하는 최소 밝기
    float4 tex = gTex.Sample(gSamp, input.uv);
    
    return float4(tex.rgb * (ambient + diffuse) + spec, 1.0f);   // 하이라이트는 흰색
}