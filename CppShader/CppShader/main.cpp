#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <d3dcompiler.h>
#include <DirectXMath.h>
#include <stdio.h>
#include <chrono>

#include "data.h"


#pragma comment(lib, "D3DCompiler.lib")
#pragma comment(lib, "d3d11.lib")

using Microsoft::WRL::ComPtr;
using namespace DirectX;

// 루프 동안 살아 있어야 하는 D3D 장치 객체들
struct Graphics
{
    ComPtr<ID3D11Device>           device;
    ComPtr<ID3D11DeviceContext>    context;
    ComPtr<IDXGISwapChain>         swapChain;
    ComPtr<ID3D11RenderTargetView> rtv;
};

// 삼각형 그리기에 필요한 리소스들
struct TriangleResources
{
    ComPtr<ID3D11Buffer>       vertexBuffer;
    ComPtr<ID3D11VertexShader> vertexShader;
    ComPtr<ID3D11PixelShader>  pixelShader;
    ComPtr<ID3D11InputLayout>  inputLayout;
};

struct CBTransform
{
    XMFLOAT4X4 wvp;
};

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

    hr = gfx.device->CreateRenderTargetView(backBuffer.Get(), nullptr, gfx.rtv.GetAddressOf());
    if (FAILED(hr))
        return hr;

    return S_OK;
}

// 정점 버퍼 ~ 셰이더 ~ 입력 레이아웃 생성 후 파이프라인에 바인딩
HRESULT SetUpTriangle(ID3D11Device* device, ID3D11DeviceContext* context, TriangleResources& tri)
{
    Vertex vertices[] =
    {
        {0.0f, 0.5f, 0.0f},
        {0.5f, -0.5f, 0.0f},
        {-0.5f, -0.5f, 0.0f},
    };

    static_assert(sizeof(Vertex) == 12);
    static_assert(offsetof(Vertex, x) == 0);
    static_assert(offsetof(Vertex, z) == 8);

    D3D11_BUFFER_DESC bufferDesc {};
    bufferDesc.ByteWidth = sizeof(vertices);
    bufferDesc.Usage = D3D11_USAGE_IMMUTABLE;
    bufferDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    bufferDesc.CPUAccessFlags = 0;

    D3D11_SUBRESOURCE_DATA initData {};
    initData.pSysMem = vertices;
    HRESULT hr = device->CreateBuffer(&bufferDesc, &initData, tri.vertexBuffer.GetAddressOf());
    if (FAILED(hr))
        return hr;

    ComPtr<ID3DBlob> vsBlob;
    ComPtr<ID3DBlob> errorBlob;
    hr = D3DCompileFromFile(L"shader.hlsl", nullptr, nullptr, "VSMain", "vs_5_0", 0, 0, vsBlob.GetAddressOf(), errorBlob.GetAddressOf());
    if (FAILED(hr))
    {
        if (errorBlob)
            OutputDebugStringA((const char*)errorBlob->GetBufferPointer());
        return hr;
    }

    hr = device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, tri.vertexShader.GetAddressOf());
    if (FAILED(hr))
        return hr;

    ComPtr<ID3DBlob> psBlob;
    hr = D3DCompileFromFile(L"shader.hlsl", nullptr, nullptr, "PSMain", "ps_5_0", 0, 0, psBlob.GetAddressOf(), errorBlob.ReleaseAndGetAddressOf());
    if (FAILED(hr))
    {
        if (errorBlob)
            OutputDebugStringA((const char*)errorBlob->GetBufferPointer());
        return hr;
    }

    hr = device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, tri.pixelShader.GetAddressOf());
    if (FAILED(hr))
        return hr;

    D3D11_INPUT_ELEMENT_DESC inputElementDesc[] =
    {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    hr = device->CreateInputLayout(inputElementDesc, _countof(inputElementDesc), vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), tri.inputLayout.GetAddressOf());
    if (FAILED(hr))
        return hr;

    UINT stride = sizeof(Vertex);
    UINT offset = 0;

    context->IASetInputLayout(tri.inputLayout.Get());
    context->IASetVertexBuffers(0, 1, tri.vertexBuffer.GetAddressOf(), &stride, &offset);
    context->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    context->VSSetShader(tri.vertexShader.Get(), nullptr, 0);
    context->PSSetShader(tri.pixelShader.Get(), nullptr, 0);
    
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
        camera.pitch = XMMax(XMMin(camera.pitch,  XMConvertToRadians(89.0f)),
                                                  XMConvertToRadians(-89.0f));
    }
    lastMouse = mouse;                       // 버튼 안 눌러도 매 프레임 갱신
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

    TriangleResources tri;
    hr = SetUpTriangle(gfx.device.Get(), gfx.context.Get(), tri);
    if (FAILED(hr))
        return 1;
    
    ComPtr<ID3D11Buffer> cbTransform;
    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = sizeof(CBTransform);
    desc.Usage = D3D11_USAGE_DYNAMIC;
    desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = gfx.device->CreateBuffer(&desc, nullptr, cbTransform.GetAddressOf());
    
    if (FAILED(hr))
        return hr;

    Camera camera;
    
    MSG message{};
    bool isRunning = true;
    const auto startTime = std::chrono::steady_clock::now();   // 회전 각도 계산용 시작 시각
    float prevTime = 0.0f; // 이전 프레임의 totalTime
    POINT lastMouse {};
    GetCursorPos(&lastMouse);
    
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
        gfx.context->OMSetRenderTargets(1, targets, nullptr);

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
            spaceHeld ? 0.0f : 1.0f,
            spaceHeld ? 0.0f : 0.05f,
            1.0f
        };
        
        // 이번 단계에서는 카메라 값을 코드에 고정해서 써요 (단계 B에서 카메라로 분리)
        // 프로그램 시작 후 흐른 시간(초)
        const float totalTime = std::chrono::duration<float>(std::chrono::steady_clock::now() - startTime).count();

        CameraMovement(camera, lastMouse, totalTime - prevTime);
        prevTime = totalTime;
        
        XMMATRIX world = XMMatrixRotationY(totalTime);
        
        CBTransform cb;
        XMStoreFloat4x4(&cb.wvp, camera.View() * camera.Projection());   // 곱셈 순서 주의, 아래 설명
        D3D11_MAPPED_SUBRESOURCE mapped;
        gfx.context->Map(cbTransform.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        memcpy(mapped.pData, &cb, sizeof(cb));
        gfx.context->Unmap(cbTransform.Get(), 0);

        gfx.context->ClearRenderTargetView(gfx.rtv.Get(), background);
        
        LogPipelineState(gfx.context.Get());
        
        gfx.context->VSSetConstantBuffers(0, 1, cbTransform.GetAddressOf()); // 슬롯 b0
        
        gfx.context->Draw(3, 0);

        hr = gfx.swapChain->Present(1, 0);
        if (FAILED(hr))
        {
            MessageBoxW(window, L"화면 표시 실패", L"오류", MB_OK);
            return 1;
        }
    }
    return 0;
}
