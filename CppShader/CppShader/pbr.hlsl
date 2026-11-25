struct VSInput  { float3 pos : POSITION; float2 uv : TEXCOORD0; float3 normal : NORMAL; float3 tangent : TANGENT; };
struct VSOutput { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; float3 normal : TEXCOORD1; float3 worldPos : TEXCOORD2; float3 tangent : TEXCOORD3; };

Texture2D    gTex  : register(t0);   // 새 변수: SRV가 연결될 슬롯 t0
SamplerState gSamp : register(s0);   // 새 변수: 샘플러가 연결될 슬롯 s0
Texture2D gNormalTex : register(t1);   // C++ 슬롯 1과 대응. 샘플러는 gSamp(s0)를 그대로 사용

cbuffer CBTransform : register(b0) // C++의 슬롯 0과 대응
{
    row_major float4x4 wvp;
    row_major float4x4 world;   // C++ 쪽 순서와 같아야 해요
    float3 cameraPos;   // C++ 필드와 같은 순서
    float  pad;
};

cbuffer CBMaterial : register(b1)
{
    float metallic;
    float roughness;
    float useAlbedoTex;
    float useNormalMap;
};
TextureCube gEnv : register(t2);   // 새 변수: 큐브맵. Texture2D가 아니라 TextureCube

VSOutput VSMain(VSInput input)
{
    VSOutput output;
    output.pos = mul(float4(input.pos, 1.0f), wvp);
    output.uv = input.uv;
    output.normal = mul(input.normal, world).xyz;
    output.worldPos = mul(float4(input.pos, 1.0f), world).xyz;
    output.tangent = mul(float4(input.tangent, 0.0f), world).xyz;
    return output;
}

float4 PSMain(VSOutput input) : SV_Target
{
    float3 raw = gNormalTex.Sample(gSamp, input.uv).rgb;   // 새 변수: 0~1로 읽힌 텍스처 색
    float3 N0 = normalize(input.normal);
    float3 T = normalize(input.tangent);
    T = normalize(T - dot(T, N0) * N0);
    float3 B = cross(N0, T);
    
    float3 nTS = raw * 2 - 1.0f;
    
    // 법선: 노멀맵 적용 전(메시 법선)과 적용 후를 useNormalMap으로 섞은 뒤 normalize
    float3 N = normalize(lerp(N0, nTS.x * T + nTS.y * B + nTS.z * N0, useNormalMap));
    
    float3 L = normalize(-float3(-1.0f, -1.0f, 0.0f));       // 빛이 가는 방향 (-1,-1,0)의 반대 = 표면에서 광원 쪽
    
    float3 V = normalize(cameraPos - input.worldPos);   // 표면 점에서 카메라 쪽
    float3 R = reflect(-L, N);                          // 3-1의 R
    float4 tex = gTex.Sample(gSamp, input.uv);
    
    // 알베도: 텍스처 색과 회색 상수(0.8)를 useAlbedoTex로 섞기
    float3 albedo = lerp(float3(0.8, 0.8, 0.8), tex, useAlbedoTex);    
    float3 F0 = lerp(0.04f, albedo, metallic);       // 3-4에서 한 식
    float3 H = normalize(L + V);                     // 새 변수: L과 V의 중간 방향
    float cosTheta = saturate(dot(H, V));
    float3 F = F0 + (1 - F0) * pow(1.0 - cosTheta, 5.0); // Schlick
    
    static const float PI = 3.14159265f;                 // 파일 맨 위(함수 밖)에 두어도 돼요

    float rough = max(roughness, 0.05f);                 // 0이면 나눗셈이 터져서 하한을 둬요 (실무 관례)
    float a  = rough * rough;                            // 새 변수: α
    float a2 = a * a;                                    // 새 변수: α²
    float NdotH = saturate(dot(N, H));                   // 새 변수: N은 노멀 맵으로 만든 그 N
    float denom = pow(NdotH, 2) * (a2 - 1) + 1;                                  // ← 직접 작성: (N·H)² · (α²−1) + 1
    float D = a2 / (PI * denom * denom);
    
    float NdotL = saturate(dot(N, L));                   // 새 변수
    float NdotV = saturate(dot(N, V));                   // 새 변수
    float k = (roughness + 1) * (roughness + 1) / 8;                                      // ← 직접 작성: (roughness + 1)² / 8
    float G1L = NdotL / (NdotL * (1.0f - k) + k);
    float G1V = NdotV / (NdotV * (1.0f - k) + k);
    float G = G1L * G1V;

    float3 spec = (D * F * G) / max(4.0f * NdotL * NdotV, 0.001f);

    float3 kD = (1.0f - F) * (1.0f - metallic);         // 새 변수: 확산으로 가는 몫 (3-4의 (1-F), metallic=1이면 0)
    float3 diffuse = kD * albedo / PI;                  // 새 변수: /π는 확산을 에너지 보존에 맞추는 값

    float3 lightColor = float3(3.0f, 3.0f, 3.0f);       // 새 변수: 광원 세기 (눈에 맞춰 조절하는 값이에요)
    float3 ambient = albedo * 0.03f;                    // 임시: 환경광 자리, 단계 C·D에서 IBL로 교체

    // float3 Renv = reflect(-V, N);                // 새 변수: 3-4에서 한 환경 반사 방향
    // float3 env = gEnv.Sample(gSamp, Renv).rgb;   // 새 변수: 그 방향의 큐브맵 색 (2D와 달리 uv 대신 방향 벡터)
    // return float4(env, 1.0f);                    // 임시: 환경 색만 출력
    
    float3 color = (diffuse + spec) * lightColor * NdotL + ambient;
    return float4(color, 1.0f);                // 임시: 지금은 조명 대신 F만 출력
}