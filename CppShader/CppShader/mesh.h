#pragma once

#include <d3d11.h>
#include <wrl/client.h>
#include <DirectXMath.h>
#include <vector>
#include "data.h"

using Microsoft::WRL::ComPtr;
using namespace DirectX;

struct Mesh                               // 새 구조체: 메시 하나를 그리는 데 필요한 GPU 객체 묶음
{
    ComPtr<ID3D11Buffer> vb, ib;
    UINT indexCount = 0;
};

// CPU 배열만 만든다. 아직 GPU 객체가 아니다.
void GenerateSphere(std::vector<Vertex>& verts, std::vector<uint16_t>& idx, int stacks, int slices)
{
    for (int i = 0; i <= stacks; ++i)
    {
        float theta = XM_PI * i / stacks;                 // 위(0) → 아래(π)
        for (int j = 0; j <= slices; ++j)
        {
            float phi = XM_2PI * j / slices;              // 한 바퀴
            float x = sin(theta) * cos(phi);                            // sinθ·cosφ
            float y = cos(theta);                                       // cosθ
            float z = sin(theta) * sin(phi);                            // sinθ·sinφ

            Vertex v{};
            v.x = 0.5f * x;  v.y = 0.5f * y;  v.z = 0.5f * z;
            v.u = (float)j / slices;  v.v = (float)i / stacks;
            v.nx = x;  v.ny = y;  v.nz = z;
            v.tx = -sinf(phi);  v.ty = 0.0f;  v.tz = cosf(phi);
            verts.push_back(v);
        }
    }
    for (int i = 0; i < stacks; ++i)
        for (int j = 0; j < slices; ++j)
        {
            uint16_t a = (uint16_t)(i * (slices + 1) + j);
            uint16_t b = (uint16_t)(a + slices + 1);
            idx.insert(idx.end(), { a, (uint16_t)(a + 1), b,   (uint16_t)(a + 1), (uint16_t)(b + 1), b });
        }
}

void GenerateCube(std::vector<Vertex>& verts, std::vector<uint16_t>& idx)
{
    struct Face { XMFLOAT3 n, t; };       // 면 법선 N, 탄젠트 T(텍스처 u 방향)
    const Face faces[6] =
    {
        {{ 0, 0,-1}, { 1, 0, 0}},   // 앞
        {{ 0, 0, 1}, {-1, 0, 0}},   // 뒤
        {{-1, 0, 0}, { 0, 0,-1}},   // 왼쪽
        {{ 1, 0, 0}, { 0, 0, 1}},   // 오른쪽
        {{ 0, 1, 0}, { 1, 0, 0}},   // 위
        {{ 0,-1, 0}, {-1, 0, 0}},   // 아래
    };
    // 면의 네 꼭짓점 UV (텍스처 기준: 좌상 → 우상 → 우하 → 좌하)
    const XMFLOAT2 cornerUV[4] = { {0, 0}, {1, 0}, {1, 1}, {0, 1} };
    const float h = 0.5f;                 // 한 변 1인 단위 큐브 (구의 반지름 0.5와 같은 기준)

    for (const Face& f : faces)
    {
        uint16_t base = (uint16_t)verts.size();   // 기존 정점 뒤에 이어 붙이기 위한 오프셋

        // B = N × T : 텍스처 v가 증가하는 방향(면 위에서 아래쪽)
        XMFLOAT3 b = { f.n.y * f.t.z - f.n.z * f.t.y,
                       f.n.z * f.t.x - f.n.x * f.t.z,
                       f.n.x * f.t.y - f.n.y * f.t.x };

        for (const XMFLOAT2& uv : cornerUV)
        {
            // UV 0~1 → 면 중심 기준 거리 -h~+h
            float alongT = (uv.x * 2.0f - 1.0f) * h;   // T(오른쪽) 방향으로 얼마나
            float alongB = (uv.y * 2.0f - 1.0f) * h;   // B(아래쪽) 방향으로 얼마나

            Vertex v{};
            // 위치 = 면 중심(N·h) + T 방향 이동 + B 방향 이동
            v.x = f.n.x * h + f.t.x * alongT + b.x * alongB;
            v.y = f.n.y * h + f.t.y * alongT + b.y * alongB;
            v.z = f.n.z * h + f.t.z * alongT + b.z * alongB;
            v.u = uv.x;  v.v = uv.y;
            v.nx = f.n.x;  v.ny = f.n.y;  v.nz = f.n.z;
            v.tx = f.t.x;  v.ty = f.t.y;  v.tz = f.t.z;
            verts.push_back(v);
        }
        idx.insert(idx.end(), { base, (uint16_t)(base + 1), (uint16_t)(base + 2),
                                base, (uint16_t)(base + 2), (uint16_t)(base + 3) });
    }
}

HRESULT CreateMesh(ComPtr<ID3D11Device> device, std::vector<Vertex>& verts, std::vector<uint16_t>& idx, Mesh& mesh)
{
    D3D11_BUFFER_DESC bufferDesc {};
    bufferDesc.ByteWidth = verts.size() * (UINT)(sizeof(Vertex));
    bufferDesc.Usage = D3D11_USAGE_IMMUTABLE;
    bufferDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    bufferDesc.CPUAccessFlags = 0;

    D3D11_SUBRESOURCE_DATA initData {};
    initData.pSysMem = verts.data();
    
    HRESULT hr = device->CreateBuffer(&bufferDesc, &initData, mesh.vb.GetAddressOf());
    if (FAILED(hr))
        return hr;
    
    D3D11_BUFFER_DESC indexBufferDesc {};
    indexBufferDesc.ByteWidth = idx.size() * (UINT)(sizeof(uint16_t));
    indexBufferDesc.Usage = D3D11_USAGE_IMMUTABLE;
    indexBufferDesc.BindFlags = D3D11_BIND_INDEX_BUFFER;
    
    D3D11_SUBRESOURCE_DATA ibData{};
    ibData.pSysMem = idx.data();
    
    hr = device->CreateBuffer(&indexBufferDesc, &ibData, mesh.ib.GetAddressOf());
    if (FAILED(hr))
        return hr;
    
    mesh.indexCount = (UINT)idx.size();
    
    return S_OK;
}
