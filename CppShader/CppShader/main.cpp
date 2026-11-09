#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <d3dcompiler.h>
#include <stdio.h>

#include "data.h"


#pragma comment(lib, "D3DCompiler.lib")
#pragma comment(lib, "d3d11.lib")

using Microsoft::WRL::ComPtr;

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
LRESULT CALLBACK WindowProc(HWND window, UINT message,
                            WPARAM wParam, LPARAM lParam)
{
    if (message == WM_DESTROY)
    {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand)
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
        return 1;

    RECT bounds{0, 0, 1280, 720};
    if (!AdjustWindowRect(&bounds, WS_OVERLAPPEDWINDOW, FALSE))
        return 1;

    HWND window = CreateWindowExW(
        0, className, L"Minimal Renderer - Window Only",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        bounds.right - bounds.left, bounds.bottom - bounds.top,
        nullptr, nullptr, instance, nullptr);

    if (!window)
        return 1;

    ShowWindow(window, showCommand);
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    
    D3D_FEATURE_LEVEL featureLevel{};
    
    UINT createFlags = 0;
#if defined(_DEBUG)
    createFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
    
    HRESULT hr = D3D11CreateDevice(
    nullptr,                    // 기본 GPU 어댑터 선택
    D3D_DRIVER_TYPE_HARDWARE,    // 하드웨어 GPU 사용
    nullptr,                    // 소프트웨어 드라이버를 사용하지 않음
    createFlags,                // Flags
    nullptr,                    // 기본 기능 수준 목록 사용
    0,                          // 직접 지정한 기능 수준 목록 없음
    D3D11_SDK_VERSION,
    device.GetAddressOf(),      // 생성된 장치를 받을 위치
    &featureLevel,              // 선택된 기능 수준을 받을 위치
    context.GetAddressOf()      // 생성된 즉시 컨텍스트를 받을 위치
);
    
    if (FAILED(hr))
    {
        MessageBoxW(
            window,
            L"Direct3D 장치 생성에 실패했습니다.",
            L"초기화 오류",
            MB_OK | MB_ICONERROR
        );
        return 1;
    }
    
    ComPtr<ID3D11Debug> d3dDebug;  // 새 변수: 디버그 인터페이스 포인터
    HRESULT hrDbg = device.As(&d3dDebug);          // device에서 디버그 인터페이스를 요청
    OutputDebugStringA(SUCCEEDED(hrDbg) ? "D3D11 debug layer: ON\n": "D3D11 debug layer: OFF\n");
    
    ComPtr<ID3D11InfoQueue> infoQueue;   // 새 변수: 디버그 메시지 대기열 인터페이스
    if (SUCCEEDED(device.As(&infoQueue)))                // d3dDebug처럼 device에서 받아오는 거라 새 객체 생성이 아님
    {
        infoQueue->SetBreakOnSeverity(D3D11_MESSAGE_SEVERITY_ERROR, TRUE);    // ERROR가 나면 디버거 정지
        infoQueue->SetBreakOnSeverity(D3D11_MESSAGE_SEVERITY_WARNING, TRUE);  // WARNING이 나면 디버거 정지
    }
    
    ComPtr<IDXGIDevice> dxgiDevice;
    hr = device.As(&dxgiDevice);
    if (FAILED(hr))
        return 1;
    
    ComPtr<IDXGIAdapter> adapter;
    hr = dxgiDevice->GetAdapter(adapter.GetAddressOf());
    if (FAILED(hr))
        return 1;
    
    ComPtr<IDXGIFactory> factory;
    hr = adapter->GetParent(IID_PPV_ARGS(factory.GetAddressOf()));
    if (FAILED(hr))
        return 1;
    
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

    ComPtr<IDXGISwapChain> swapChain;
    hr = factory->CreateSwapChain(
        device.Get(),
        &desc,
        swapChain.GetAddressOf()
    );
    
    if (FAILED(hr))
    {
        MessageBoxW(window, L"스왑 체인 생성 실패", L"오류", MB_OK);
        return 1;
    }
    
    ComPtr<ID3D11Texture2D> backBuffer;
    hr = swapChain->GetBuffer(0, IID_PPV_ARGS(backBuffer.GetAddressOf()));
    if (FAILED(hr))
        return 1;
    
    ComPtr<ID3D11RenderTargetView> rtv;
    hr = device->CreateRenderTargetView(backBuffer.Get(), nullptr, rtv.GetAddressOf());
    if (FAILED(hr))
        return 1;
    
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
    ComPtr<ID3D11Buffer> vertexBuffer;
    hr = device->CreateBuffer(&bufferDesc, &initData, vertexBuffer.GetAddressOf());
    if (FAILED(hr))
        return 1;
    
    ComPtr<ID3DBlob> vsBlob;
    ComPtr<ID3DBlob> errorBlob;
    hr = D3DCompileFromFile(L"shader.hlsl", nullptr, nullptr, "VSMain", "vs_5_0", 0, 0, vsBlob.GetAddressOf(), errorBlob.GetAddressOf());
    if (FAILED(hr))
    {
        if (errorBlob)
            OutputDebugStringA((const char*)errorBlob->GetBufferPointer());
        return 1;
    }
    
    ComPtr<ID3D11VertexShader> vertexShader;
    hr = device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, vertexShader.GetAddressOf());
    if (FAILED(hr))
        return 1;

    ComPtr<ID3DBlob> psBlob;
    hr = D3DCompileFromFile(L"shader.hlsl", nullptr, nullptr, "PSMain", "ps_5_0", 0, 0, psBlob.GetAddressOf(), errorBlob.GetAddressOf());
    if (FAILED(hr))
    {
        if (errorBlob)
            OutputDebugStringA((const char*)errorBlob->GetBufferPointer());
        return 1;
    }
    
    ComPtr<ID3D11PixelShader> pixelShader;
    hr = device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, pixelShader.GetAddressOf());
    if (FAILED(hr))
        return 1;
    
    D3D11_INPUT_ELEMENT_DESC inputElementDesc[] = 
    {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},    
    };
    ComPtr<ID3D11InputLayout> inputLayout;
    hr = device->CreateInputLayout(inputElementDesc, _countof(inputElementDesc), vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), inputLayout.GetAddressOf());
    if (FAILED(hr))
        return 1;

    UINT stride = sizeof(Vertex);
    UINT offset = 0;
    
    context->IASetInputLayout(inputLayout.Get());
    context->IASetVertexBuffers(0, 1, vertexBuffer.GetAddressOf(), &stride, &offset);
    context->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    
    context->VSSetShader(vertexShader.Get(), nullptr, 0);
    context->PSSetShader(pixelShader.Get(), nullptr, 0);
    
    MSG message{};
    bool isRunning = true;
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
        ID3D11RenderTargetView* targets[] = { rtv.Get() };
        context->OMSetRenderTargets(1, targets, nullptr);
        
        D3D11_VIEWPORT viewport{};
        viewport.TopLeftX = 0.0f;
        viewport.TopLeftY = 0.0f;
        viewport.Width    = 1280.0f;
        viewport.Height   = 720.0f;
        viewport.MinDepth = 0.0f;     // 깊이 범위, 지금은 이 값 그대로 (2-3에서 다룸)
        viewport.MaxDepth = 1.0f;

        context->RSSetViewports(1, &viewport);

        const bool spaceHeld = (GetAsyncKeyState(VK_SPACE) & 0x8000) != 0;
        
        const float background[] = {
            spaceHeld ? 1.0f : 0.0f,
            spaceHeld ? 0.0f : 1.0f,
            spaceHeld ? 0.0f : 0.05f,
            1.0f
        };
        
        context->ClearRenderTargetView(rtv.Get(), background);
        LogPipelineState(context.Get());
        context->Draw(3, 0);
        
        hr = swapChain->Present(1, 0);
        if (FAILED(hr))
        {
            MessageBoxW(window, L"화면 표시 실패", L"오류", MB_OK);
            return 1;
        }
    }
    return 0;
}
