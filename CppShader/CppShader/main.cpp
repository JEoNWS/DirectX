#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include <chrono>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#include "mesh.h"

#pragma comment(lib, "D3DCompiler.lib")
#pragma comment(lib, "d3d11.lib")

// 루프 동안 살아 있어야 하는 D3D 장치 객체들
struct Graphics
{
    ComPtr<ID3D11Device>           device;
    ComPtr<ID3D11DeviceContext>    context;
    ComPtr<IDXGISwapChain>         swapChain;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11Texture2D>        depthStencil;
    ComPtr<ID3D11DepthStencilView> dsv;
    ComPtr<ID3D11DepthStencilState> depthStencilState;
};

struct ShaderSet                          // 새 구조체: 한 파일에서 나온 VS+PS 한 쌍
{
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader>  ps;
};

// 삼각형 그리기에 필요한 리소스들
struct MeshResources
{
    Mesh mesh;
    ShaderSet phong, pbr;
    ComPtr<ID3D11InputLayout>  inputLayout;
    ComPtr<ID3D11RasterizerState> rasterizerState;
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11ShaderResourceView> textureSRV;
    ComPtr<ID3D11SamplerState> samplerState;
    ComPtr<ID3D11Texture2D> normalTexture;
    ComPtr<ID3D11ShaderResourceView> normalTextureSRV;
    ComPtr<ID3D11Texture2D> cubeTex;                             // 새 변수: 큐브맵 텍스처
    ComPtr<ID3D11ShaderResourceView> cubeSRV;                    // 새 변수: 셰이더가 읽을 뷰
};

struct CBTransform
{
    XMFLOAT4X4 wvp;
    XMFLOAT4X4 world;   // 새 필드: 월드 행렬 (N을 월드 공간으로 바꾸는 데 쓰려고)
    XMFLOAT3   cameraPos;   // 새 필드: 월드 공간 카메라 위치 (V 계산용)
    float      pad;         // 상수 버퍼 크기를 16의 배수로 맞추는 패딩
};

struct CBMaterial
{
    float metallic; 
    float roughness; 
    float useAlbedoTex;   // 새 필드: 1이면 texture.png 사용, 0이면 회색 상수
    float useNormalMap;   // 새 필드: 1이면 노멀맵 사용, 0이면 메시 법선 그대로
};  // 상수 버퍼는 16바이트 단위

struct Camera
{
    XMFLOAT3 position = { 1.0f, 0.0f, -3.0f };
    float yaw = 0.0f;
    float pitch = 0.0f;
    
    float fovY = XMConvertToRadians(60.0f);
    float aspect = 1280.0f / 720.0f;
    float nearZ = 0.1f;
    float farZ = 100.0f;
    
    XMVECTOR Forward() const
    {
        return XMVectorSet(cosf(pitch) * sinf(yaw), sinf(pitch), cosf(pitch) * cosf(yaw), 0.0f);
    }
    
    XMMATRIX View() const
    {
        return XMMatrixLookToLH(XMLoadFloat3(&position), Forward(), XMVectorSet(0, 1, 0, 0));
    }
    
    XMMATRIX Projection() const
    {
        return XMMatrixPerspectiveFovLH(fovY, aspect, nearZ, farZ);
    }
};

void LogPipelineState(ID3D11DeviceContext* ctx)   // ctx: 이미 만든 컨텍스트를 인자로 받음
{
    ComPtr<ID3D11PixelShader> ps;   // 읽어온 PS를 받을 변수 (새로 만드는 게 아님)
    ctx->PSGetShader(ps.GetAddressOf(), nullptr, nullptr);
    OutputDebugStringA(ps ? "PS: bound\n" : "PS: NULL\n");

    ComPtr<ID3D11InputLayout> layout;
    ctx->IAGetInputLayout(layout.GetAddressOf());
    OutputDebugStringA(layout ? "InputLayout: bound\n" : "InputLayout: NULL\n");

    D3D11_PRIMITIVE_TOPOLOGY topo;
    ctx->IAGetPrimitiveTopology(&topo);
    char buf[64];
    sprintf_s(buf, "Topology: %d\n", (int)topo);
    OutputDebugStringA(buf);
}

// Windows calls this function when a message reaches our window.
LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_DESTROY)
    {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

// 창 클래스 등록 ~ 창 생성/표시
HRESULT CreateMainWindow(HINSTANCE instance, int showCommand, HWND& window)
{
    constexpr wchar_t className[] = L"MinimalRendererWindow";

    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = className;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    // Temporary Windows background brush, NOT DirectX rendering.
    windowClass.hbrBackground = GetSysColorBrush(COLOR_WINDOW);

    if (!RegisterClassW(&windowClass))
        return HRESULT_FROM_WIN32(GetLastError());

    RECT bounds{0, 0, 1280, 720};
    if (!AdjustWindowRect(&bounds, WS_OVERLAPPEDWINDOW, FALSE))
        return HRESULT_FROM_WIN32(GetLastError());

    window = CreateWindowExW(
        0, className, L"Minimal Renderer - Window Only",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        bounds.right - bounds.left, bounds.bottom - bounds.top,
        nullptr, nullptr, instance, nullptr);

    if (!window)
        return HRESULT_FROM_WIN32(GetLastError());

    ShowWindow(window, showCommand);

    return S_OK;
}

// D3D 장치 생성 ~ 렌더 타깃 뷰 생성
HRESULT InitD3D(HWND window, Graphics& gfx)
{
    D3D_FEATURE_LEVEL featureLevel{};

    UINT createFlags = 0;
#if defined(_DEBUG)
    createFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    HRESULT hr = D3D11CreateDevice(
        nullptr,                        // 기본 GPU 어댑터 선택
        D3D_DRIVER_TYPE_HARDWARE,       // 하드웨어 GPU 사용
        nullptr,                        // 소프트웨어 드라이버를 사용하지 않음
        createFlags,                    // Flags
        nullptr,                        // 기본 기능 수준 목록 사용
        0,                              // 직접 지정한 기능 수준 목록 없음
        D3D11_SDK_VERSION,
        gfx.device.GetAddressOf(),      // 생성된 장치를 받을 위치
        &featureLevel,                  // 선택된 기능 수준을 받을 위치
        gfx.context.GetAddressOf()      // 생성된 즉시 컨텍스트를 받을 위치
    );

    if (FAILED(hr))
    {
        MessageBoxW(
            window,
            L"Direct3D 장치 생성에 실패했습니다.",
            L"초기화 오류",
            MB_OK | MB_ICONERROR
        );
        return hr;
    }

    ComPtr<ID3D11Debug> d3dDebug;  // 새 변수: 디버그 인터페이스 포인터
    HRESULT hrDbg = gfx.device.As(&d3dDebug);          // device에서 디버그 인터페이스를 요청
    OutputDebugStringA(SUCCEEDED(hrDbg) ? "D3D11 debug layer: ON\n": "D3D11 debug layer: OFF\n");

    ComPtr<ID3D11InfoQueue> infoQueue;   // 새 변수: 디버그 메시지 대기열 인터페이스
    if (SUCCEEDED(gfx.device.As(&infoQueue)))            // d3dDebug처럼 device에서 받아오는 거라 새 객체 생성이 아님
    {
        infoQueue->SetBreakOnSeverity(D3D11_MESSAGE_SEVERITY_ERROR, TRUE);    // ERROR가 나면 디버거 정지
        infoQueue->SetBreakOnSeverity(D3D11_MESSAGE_SEVERITY_WARNING, TRUE);  // WARNING이 나면 디버거 정지
    }

    ComPtr<IDXGIDevice> dxgiDevice;
    hr = gfx.device.As(&dxgiDevice);
    if (FAILED(hr))
        return hr;

    ComPtr<IDXGIAdapter> adapter;
    hr = dxgiDevice->GetAdapter(adapter.GetAddressOf());
    if (FAILED(hr))
        return hr;

    ComPtr<IDXGIFactory> factory;
    hr = adapter->GetParent(IID_PPV_ARGS(factory.GetAddressOf()));
    if (FAILED(hr))
        return hr;

    DXGI_SWAP_CHAIN_DESC desc{};
    desc.BufferDesc.Width = 1280;
    desc.BufferDesc.Height = 720;
    desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.OutputWindow = window;
    desc.Windowed = TRUE;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    hr = factory->CreateSwapChain(
        gfx.device.Get(),
        &desc,
        gfx.swapChain.GetAddressOf()
    );

    if (FAILED(hr))
    {
        MessageBoxW(window, L"스왑 체인 생성 실패", L"오류", MB_OK);
        return hr;
    }

    ComPtr<ID3D11Texture2D> backBuffer;
    hr = gfx.swapChain->GetBuffer(0, IID_PPV_ARGS(backBuffer.GetAddressOf()));
    if (FAILED(hr))
        return hr;

    D3D11_RENDER_TARGET_VIEW_DESC rtvDesc{};                 // 새 변수: RTV를 어떤 포맷/차원으로 만들지 지정
    rtvDesc.Format        = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    
    hr = gfx.device->CreateRenderTargetView(backBuffer.Get(), &rtvDesc, gfx.rtv.GetAddressOf());
    if (FAILED(hr))
        return hr;
    
    D3D11_DEPTH_STENCIL_DESC dsDesc{};
    dsDesc.DepthEnable    = TRUE;                          // 깊이 검사를 할까?
    dsDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;   // 통과하면 깊이를 쓸까? (ALL / ZERO)
    dsDesc.DepthFunc      = D3D11_COMPARISON_LESS;         // 어떤 비교로 통과시킬까?
    dsDesc.StencilEnable  = FALSE;
    
    hr = gfx.device->CreateDepthStencilState(&dsDesc, gfx.depthStencilState.GetAddressOf());
    if (FAILED(hr))
        return hr;
    
    D3D11_TEXTURE2D_DESC depthDesc {};
    depthDesc.Width = 1280;
    depthDesc.Height = 720;
    depthDesc.MipLevels = 1;
    depthDesc.ArraySize = 1;
    depthDesc.Format = DXGI_FORMAT_D32_FLOAT;
    depthDesc.SampleDesc.Count = 1;
    depthDesc.Usage = D3D11_USAGE_DEFAULT;
    depthDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    
    hr = gfx.device->CreateTexture2D(&depthDesc, nullptr, gfx.depthStencil.GetAddressOf());
    if (FAILED(hr))
        return hr;
    hr = gfx.device->CreateDepthStencilView(gfx.depthStencil.Get(), nullptr, gfx.dsv.GetAddressOf());
    if (FAILED(hr))
        return hr;

    return S_OK;
}

HRESULT CreateShaderSet(ID3D11Device* device, const wchar_t* path,
                        ShaderSet& out, ID3DBlob** vsBlobOut = nullptr)
{
    ComPtr<ID3DBlob> vsBlob, psBlob, errorBlob;

    HRESULT hr = D3DCompileFromFile(path, nullptr, nullptr, "VSMain", "vs_5_0", 0, 0,
                                    vsBlob.GetAddressOf(), errorBlob.GetAddressOf());
    if (FAILED(hr))
    {
        if (errorBlob) OutputDebugStringA((const char*)errorBlob->GetBufferPointer());
        else           OutputDebugStringA("shader compile failed (no error message: file not found?)\n");
        return hr;
    }
    hr = device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(),
                                    nullptr, out.vs.GetAddressOf());
    if (FAILED(hr)) return hr;

    hr = D3DCompileFromFile(path, nullptr, nullptr, "PSMain", "ps_5_0", 0, 0,
                            psBlob.GetAddressOf(), errorBlob.ReleaseAndGetAddressOf());
    if (FAILED(hr))
    {
        if (errorBlob) OutputDebugStringA((const char*)errorBlob->GetBufferPointer());
        else           OutputDebugStringA("shader compile failed (no error message: file not found?)\n");
        return hr;
    }
    hr = device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(),
                                   nullptr, out.ps.GetAddressOf());
    if (FAILED(hr)) return hr;

    if (vsBlobOut) vsBlob.CopyTo(vsBlobOut);   // 호출한 쪽도 blob을 쓸 수 있게 참조를 하나 더 넘김
    return S_OK;
}

// 정점 버퍼 ~ 셰이더 ~ 입력 레이아웃 생성 후 파이프라인에 바인딩
HRESULT SetUpMeshes(ID3D11Device* device, ID3D11DeviceContext* context, MeshResources& mesh)
{
    ComPtr<ID3DBlob> layoutBlob;
    ShaderSet phong;
    ShaderSet pbr;
    HRESULT hr = CreateShaderSet(device, L"pbr.hlsl", pbr, layoutBlob.GetAddressOf());
    if (FAILED(hr))
        return hr;
    mesh.pbr = pbr;
    
    hr = CreateShaderSet(device, L"phong.hlsl", phong);
    if (FAILED(hr))
        return hr;
    mesh.phong = phong;
    
    D3D11_INPUT_ELEMENT_DESC inputElementDesc[] =
    {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(Vertex, u), D3D11_INPUT_PER_VERTEX_DATA},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(Vertex, nx), D3D11_INPUT_PER_VERTEX_DATA},
        { "TANGENT", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(Vertex, tx), D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    hr = device->CreateInputLayout(inputElementDesc, _countof(inputElementDesc), layoutBlob->GetBufferPointer(), layoutBlob->GetBufferSize(), mesh.inputLayout.GetAddressOf());
    if (FAILED(hr))
        return hr;
    
    UINT stride = sizeof(Vertex);
    UINT offset = 0;
    
    D3D11_RASTERIZER_DESC rsDesc{};
    rsDesc.FillMode              = D3D11_FILL_SOLID;
    rsDesc.CullMode              = D3D11_CULL_BACK;
    rsDesc.FrontCounterClockwise = FALSE;
    rsDesc.DepthClipEnable       = TRUE;
    
    hr = device->CreateRasterizerState(&rsDesc, mesh.rasterizerState.GetAddressOf());
    if (FAILED(hr))
        return hr;
    
    int w = 0, h = 0, channels = 0;                                 // 새 변수: 이미지 크기와 원래 채널 수를 받을 자리
    unsigned char* data = stbi_load("texture.png", &w, &h, &channels, 4);  // 새 변수: 디코딩된 픽셀의 CPU 메모리 주소
    if (data == nullptr)
    {
        OutputDebugStringA(stbi_failure_reason());
        return E_FAIL;
    }
    
    D3D11_TEXTURE2D_DESC texDesc{};
    texDesc.Width            = w;
    texDesc.Height           = h;
    texDesc.MipLevels        = 1;
    texDesc.ArraySize        = 1;
    texDesc.Format           = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage            = D3D11_USAGE_IMMUTABLE;
    texDesc.BindFlags        = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA texData{};
    texData.pSysMem     = data;
    texData.SysMemPitch = w * 4;     // 새로 등장하는 필드
    
    hr = device->CreateTexture2D(&texDesc, &texData, mesh.texture.GetAddressOf());
    if (FAILED(hr))
        return hr;
    
    stbi_image_free(data);
    
    hr = device->CreateShaderResourceView(mesh.texture.Get(), nullptr, mesh.textureSRV.GetAddressOf()); 
    if (FAILED(hr))
        return hr;
    
    D3D11_SAMPLER_DESC sampDesc{};
    sampDesc.Filter   = D3D11_FILTER_MIN_MAG_MIP_POINT;   // 일단 이걸로
    sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
    sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
    sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    sampDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sampDesc.MaxLOD   = D3D11_FLOAT32_MAX;
    
    hr = device->CreateSamplerState(&sampDesc, mesh.samplerState.GetAddressOf());
    if (FAILED(hr))
        return hr;
    
    int nw = 0, nh = 0, nch = 0;
    unsigned char* nData = stbi_load("normal.png", &nw, &nh, &nch, 4);
    if (nData == nullptr) { OutputDebugStringA(stbi_failure_reason()); return E_FAIL; }

    D3D11_TEXTURE2D_DESC nDesc{};
    nDesc.Width            = nw;
    nDesc.Height           = nh;
    nDesc.MipLevels        = 1;
    nDesc.ArraySize        = 1;
    nDesc.Format           = DXGI_FORMAT_R8G8B8A8_UNORM;   // ← _SRGB 아님
    nDesc.SampleDesc.Count = 1;
    nDesc.Usage            = D3D11_USAGE_IMMUTABLE;
    nDesc.BindFlags        = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA nInit{};
    nInit.pSysMem     = nData;
    nInit.SysMemPitch = nw * 4;

    hr = device->CreateTexture2D(&nDesc, &nInit, mesh.normalTexture.GetAddressOf());
    stbi_image_free(nData);          // 복사된 뒤라 해제 가능 (D-4에서 확인한 내용)
    if (FAILED(hr)) 
        return hr;
    
    const char* faceFiles[6] = { "cube_px.png", "cube_nx.png", "cube_py.png",
                             "cube_ny.png", "cube_pz.png", "cube_nz.png" };   // D3D11 면 순서: +X, -X, +Y, -Y, +Z, -Z

    stbi_uc* facePixels[6] = {};                  // 새 변수: 면별 픽셀 포인터
    int comp = 0;
    for (int i = 0; i < 6; ++i)
    {
        facePixels[i] = stbi_load(faceFiles[i], &w, &h, &comp, 4);
        if (!facePixels[i])
        {
            if (facePixels[i] == nullptr) { OutputDebugStringA(stbi_failure_reason()); return E_FAIL; }
        }
    }

    D3D11_TEXTURE2D_DESC cubeDesc{};
    cubeDesc.Width = w;
    cubeDesc.Height = h;
    cubeDesc.MipLevels = 1;
    cubeDesc.ArraySize = 6;
    cubeDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;           // 색 이미지라 texture.png와 같은 이유로 _SRGB
    cubeDesc.SampleDesc.Count = 1;
    cubeDesc.Usage = D3D11_USAGE_IMMUTABLE;
    cubeDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    cubeDesc.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE;

    D3D11_SUBRESOURCE_DATA cubeInit[6] = {};                     // 새 변수: 면마다 하나씩
    for (int i = 0; i < 6; ++i)
    {
        cubeInit[i].pSysMem = facePixels[i];
        cubeInit[i].SysMemPitch = w * 4;                          // ← 직접 작성
    }

    hr = device->CreateTexture2D(&cubeDesc, cubeInit, mesh.cubeTex.GetAddressOf());
    // HRESULT 검사 필요
    if (FAILED(hr))
        return hr;
    
    for (int i = 0; i < 6; ++i) stbi_image_free(facePixels[i]);  // CreateTexture2D 뒤라 안전

    D3D11_SHADER_RESOURCE_VIEW_DESC cubeSrvDesc{};
    cubeSrvDesc.Format = cubeDesc.Format;
    cubeSrvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;                            // ← 직접 작성: TEXTURECUBE
    cubeSrvDesc.TextureCube.MostDetailedMip = 0;
    cubeSrvDesc.TextureCube.MipLevels = 1;

    hr = device->CreateShaderResourceView(mesh.cubeTex.Get(), &cubeSrvDesc, mesh.cubeSRV.GetAddressOf());
    if (FAILED(hr))
        return hr;
    
    hr = device->CreateShaderResourceView(mesh.normalTexture.Get(), nullptr, mesh.normalTextureSRV.GetAddressOf());
    if (FAILED(hr)) return hr;

    context->IASetInputLayout(mesh.inputLayout.Get());
    context->IASetVertexBuffers(0, 1, mesh.mesh.vb.GetAddressOf(), &stride, &offset);
    context->IASetIndexBuffer(mesh.mesh.ib.Get(), DXGI_FORMAT_R16_UINT, 0);
    context->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->RSSetState(mesh.rasterizerState.Get());

    context->PSSetShaderResources(0, 1, mesh.textureSRV.GetAddressOf());
    context->PSSetShaderResources(1, 1, mesh.normalTextureSRV.GetAddressOf());   // t1
    context->PSSetSamplers(0, 1, mesh.samplerState.GetAddressOf());
    
    context->PSSetShaderResources(2, 1, mesh.cubeSRV.GetAddressOf());   // t2
    return S_OK;
}

void CameraMovement(Camera& camera, POINT& lastMouse, float dt)
{
    const float speed = 2.0f;                // 초당 2 유닛
    XMVECTOR pos   = XMLoadFloat3(&camera.position);
    XMVECTOR fwd   = camera.Forward();
    XMVECTOR right = XMVector3Normalize(XMVector3Cross(XMVectorSet(0, 1, 0, 0), fwd));  // 왼손 좌표계에서 오른쪽 방향

    if (GetAsyncKeyState('W') & 0x8000) pos += fwd   * (speed * dt);
    if (GetAsyncKeyState('S') & 0x8000) pos -= fwd   * (speed * dt);
    if (GetAsyncKeyState('D') & 0x8000) pos += right * (speed * dt);
    if (GetAsyncKeyState('A') & 0x8000) pos -= right * (speed * dt);

    XMStoreFloat3(&camera.position, pos);
    
    POINT mouse;
    GetCursorPos(&mouse);
    if (GetAsyncKeyState(VK_RBUTTON) & 0x8000)
    {
        const float sensitivity = 0.0025f;   // 픽셀당 라디안
        camera.yaw   += (mouse.x - lastMouse.x) * sensitivity;
        camera.pitch -= (mouse.y - lastMouse.y) * sensitivity;   // 마우스를 내리면 아래를 봐요
        camera.pitch = XMMax(XMMin(camera.pitch,  XMConvertToRadians(89.0f)), XMConvertToRadians(-89.0f));
    }
    lastMouse = mouse;                       // 버튼 안 눌러도 매 프레임 갱신
}

template <typename T>
void UpdateCB(ID3D11DeviceContext* ctx, ID3D11Buffer* buf, const T& data)   // 새 함수: 상수 버퍼 내용 덮어쓰기
{
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ctx->Map(buf, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
        return;
    memcpy(m.pData, &data, sizeof(T));
    ctx->Unmap(buf, 0);
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand)
{
    HWND window = nullptr;
    HRESULT hr = CreateMainWindow(instance, showCommand, window);
    if (FAILED(hr))
        return 1;

    Graphics gfx;   // tri보다 먼저 선언 → 종료 시 tri가 먼저 해제되고 device는 마지막에 해제됨
    hr = InitD3D(window, gfx);
    if (FAILED(hr))
        return 1;
    
    MeshResources sphere;
    uint16_t stack = 16;
    uint16_t slice = 32;
    std::vector<Vertex> sphereVert;
    std::vector<uint16_t> sphereIdx;
    GenerateSphere(sphereVert, sphereIdx, stack, slice);
    hr = CreateMesh(gfx.device, sphereVert, sphereIdx, sphere.mesh);
    if (FAILED(hr)) return 1;
    hr = SetUpMeshes(gfx.device.Get(), gfx.context.Get(), sphere);
    if (FAILED(hr)) return 1;
    
    MeshResources cube;
    std::vector<Vertex> cubeVert;
    std::vector<uint16_t> cubeIdx;
    GenerateCube(cubeVert, cubeIdx);
    hr = CreateMesh(gfx.device, cubeVert, cubeIdx, cube.mesh);
    if (FAILED(hr)) return 1;
    hr = SetUpMeshes(gfx.device.Get(), gfx.context.Get(), cube);
    if (FAILED(hr)) return 1;
    
    ComPtr<ID3D11Buffer> cbTransform;
    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = sizeof(CBTransform);
    desc.Usage = D3D11_USAGE_DYNAMIC;
    desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = gfx.device->CreateBuffer(&desc, nullptr, cbTransform.GetAddressOf());
    
    if (FAILED(hr))
        return hr;

    ComPtr<ID3D11Buffer> cbMaterial;                 // 새 변수: 재질 값을 담는 GPU 버퍼
    CBMaterial mat{ 0.0f, 0.5f, 1.0f, 1.0f};            // 새 변수: 초기 재질 (비금속, 중간 거칠기)
    
    D3D11_BUFFER_DESC matDesc{};
    matDesc.ByteWidth = sizeof(CBMaterial);
    matDesc.Usage = D3D11_USAGE_DYNAMIC;
    matDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    matDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    
    D3D11_SUBRESOURCE_DATA matData{};
    matData.pSysMem = &mat;
    hr = gfx.device->CreateBuffer(&matDesc, &matData, cbMaterial.GetAddressOf());
    if (FAILED(hr))
        return hr;
    
    Camera camera;
    
    MSG message{};
    bool isRunning = true;
    const auto startTime = std::chrono::steady_clock::now();   // 회전 각도 계산용 시작 시각
    float prevTime = 0.0f; // 이전 프레임의 totalTime
    POINT lastMouse {};
    GetCursorPos(&lastMouse);
    
    bool usePbr = false;                                  // 루프 밖: 지금 어느 쪽을 쓰는지 기억
    bool useSphere = false;
    bool useGrid = false;
    MeshResources curMesh = cube;
    
    while (isRunning)
    {
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
        {
            if (message.message == WM_QUIT)
            {
                isRunning = false;
                break;
            }

            TranslateMessage(&message);
            DispatchMessageW(&message);
        }

        if (!isRunning)
            break;

        // 최소화 상태에서는 렌더링을 쉬어요.
        if (IsIconic(window))
        {
            WaitMessage();
            continue;
        }

        // 플립 방식의 Present 이후에는 다시 연결해 줍니다.
        ID3D11RenderTargetView* targets[] = { gfx.rtv.Get() };
        gfx.context->OMSetRenderTargets(1, targets, gfx.dsv.Get());
        gfx.context->OMSetDepthStencilState(gfx.depthStencilState.Get(), 0);

        D3D11_VIEWPORT viewport{};
        viewport.TopLeftX = 0.0f;
        viewport.TopLeftY = 0.0f;
        viewport.Width    = 1280.0f;
        viewport.Height   = 720.0f;
        viewport.MinDepth = 0.0f;     // 깊이 범위, 지금은 이 값 그대로 (2-3에서 다룸)
        viewport.MaxDepth = 1.0f;

        gfx.context->RSSetViewports(1, &viewport);

        const bool spaceHeld = (GetAsyncKeyState(VK_SPACE) & 0x8000) != 0;

        const float background[] = {
            spaceHeld ? 1.0f : 0.0f,
            spaceHeld ? 0.0f : 0.0f,
            spaceHeld ? 0.0f : 0.05f,
            1.0f
        };
        
        // 이번 단계에서는 카메라 값을 코드에 고정해서 써요 (단계 B에서 카메라로 분리)
        // 프로그램 시작 후 흐른 시간(초)
        const float totalTime = std::chrono::duration<float>(std::chrono::steady_clock::now() - startTime).count();
        const float dt = totalTime - prevTime;          // 새 변수: 이번 프레임에 흐른 시간(초)
        CameraMovement(camera, lastMouse, totalTime - prevTime);
        prevTime = totalTime;
        XMMATRIX world = XMMatrixRotationY(totalTime);
        CBTransform cb;
        XMStoreFloat4x4(&cb.wvp, world * camera.View() * camera.Projection());   // 곱셈 순서 주의, 아래 설명
        XMStoreFloat4x4(&cb.world, world);
        cb.cameraPos = camera.position;   // camera는 루프에서 이미 쓰는 Camera 객체
        UpdateCB<CBTransform>(gfx.context.Get(), cbTransform.Get(), cb);

        gfx.context->ClearRenderTargetView(gfx.rtv.Get(), background);
        gfx.context->ClearDepthStencilView(gfx.dsv.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);
        
        // 재질 조절 (↑↓ = metallic, ←→ = roughness)
        if (GetAsyncKeyState(VK_UP)    & 0x8000) mat.metallic  += 0.5f * dt;
        if (GetAsyncKeyState(VK_DOWN)  & 0x8000) mat.metallic  -= 0.5f * dt;
        if (GetAsyncKeyState(VK_RIGHT) & 0x8000) mat.roughness += 0.5f * dt;
        if (GetAsyncKeyState(VK_LEFT)  & 0x8000) mat.roughness -= 0.5f * dt;
        mat.metallic = max(0, min(1, mat.metallic)); // 0~1로 제한 (카메라 pitch 제한 코드와 같은 방식)
        mat.roughness = max(0, min(1, mat.roughness));
        UpdateCB<CBMaterial>(gfx.context.Get(), cbMaterial.Get(), mat);
        
        // 지금 어떤 값인지 창 제목에 표시 (보고할 때 편해요)
        wchar_t title[128];
        swprintf_s(title, L"%s  metallic=%.2f  roughness=%.2f", usePbr ? L"PBR" : L"Phong", mat.metallic, mat.roughness);
        SetWindowTextW(window, title);
        
        LogPipelineState(gfx.context.Get());
        
        gfx.context->VSSetConstantBuffers(0, 1, cbTransform.GetAddressOf()); // 슬롯 b0
        
        gfx.context->PSSetConstantBuffers(0, 1, cbTransform.GetAddressOf());  // PS도 b0(cameraPos)를 읽어요
        gfx.context->PSSetConstantBuffers(1, 1, cbMaterial.GetAddressOf());   // b1 = 재질
        
        // 루프 안, DrawIndexed 전
        if (GetAsyncKeyState('1') & 0x8000) usePbr = false;
        if (GetAsyncKeyState('2') & 0x8000) usePbr = true;
        if (GetAsyncKeyState('3') & 0x8000) { useGrid = false; useSphere = false; }
        if (GetAsyncKeyState('4') & 0x8000) { useGrid = false; useSphere = true;  }
        if (GetAsyncKeyState('5') & 0x8000) { useGrid = true; }
        
        const float useTex = (GetAsyncKeyState('T') & 0x8000) ? 0.0f : 1.0f;   // T를 누르고 있는 동안 알베도 텍스처 끔
        const float useNrm = (GetAsyncKeyState('N') & 0x8000) ? 0.0f : 1.0f;   // N을 누르고 있는 동안 노멀맵 끔
        mat.useAlbedoTex = useTex;  mat.useNormalMap = useNrm;
        
        const ShaderSet& cur = usePbr ? curMesh.pbr : curMesh.phong;  // 새 변수: 이번 프레임에 쓸 세트 (복사 아님, 참조)
        gfx.context->VSSetShader(cur.vs.Get(), nullptr, 0);
        gfx.context->PSSetShader(cur.ps.Get(), nullptr, 0);
        
        UINT stride = sizeof(Vertex), offset = 0;                 // 루프 안에서 새로 선언 (SetUpTriangle의 것과 별개 변수)
        if (useGrid)
        {
            gfx.context->IASetVertexBuffers(0, 1, sphere.mesh.vb.GetAddressOf(), &stride, &offset);
            gfx.context->IASetIndexBuffer(sphere.mesh.ib.Get(), DXGI_FORMAT_R16_UINT, 0);

            const int N = 5;                 // 5×5
            const float spacing = 1.2f;      // 구 지름이 1.0이라 약간 띄움
            const float mid = (N - 1) * 0.5f;           // 가운데 인덱스 = 2.0
            
            for (int row = 0; row < N; ++row)           // 세로 = roughness (위쪽이 매끈)
                for (int col = 0; col < N; ++col)       // 가로 = metallic (왼쪽이 비금속)
                {
                    CBMaterial m{ col / (float)(N - 1),
                        0.1f + 0.9f * row / (N - 1), mat.useAlbedoTex, mat.useNormalMap };                 // metallic: 0~1을 5단계, roughness: 0.1~1.0 5단계
                    XMMATRIX w = XMMatrixTranslation((col - mid) * spacing, 
                        (mid - row) * spacing, 0.0f); // 격자 중앙이 원점이 되게 (x는 col, y는 row 기준)

                    CBTransform t;
                    XMStoreFloat4x4(&t.wvp, w * camera.View() * camera.Projection());
                    XMStoreFloat4x4(&t.world, w);
                    t.cameraPos = camera.position;

                    UpdateCB(gfx.context.Get(), cbTransform.Get(), t);
                    UpdateCB(gfx.context.Get(), cbMaterial.Get(), m);
                    gfx.context->DrawIndexed(sphere.mesh.indexCount, 0, 0);
                }
        }
        else
        {
            if (useSphere)
                curMesh = sphere;
            else
                curMesh = cube;
            gfx.context->IASetVertexBuffers(0, 1, curMesh.mesh.vb.GetAddressOf(), &stride, &offset);
            gfx.context->IASetIndexBuffer(curMesh.mesh.ib.Get(), DXGI_FORMAT_R16_UINT, 0);
            gfx.context->DrawIndexed(curMesh.mesh.indexCount, 0, 0);
        }
        
        hr = gfx.swapChain->Present(1, 0);
        if (FAILED(hr))
        {
            MessageBoxW(window, L"화면 표시 실패", L"오류", MB_OK);
            return 1;
        }
    }
    return 0;
}
