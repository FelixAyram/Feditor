struct FrameCB {
    float4x4 view_proj;
    float4x4 object;
};

cbuffer CB : register(b0) {
    FrameCB g_frame;
};

struct VSIn {
    float3 pos : POSITION;
    float3 nrm : NORMAL;
    float2 uv  : TEXCOORD0;
};

struct VSOut {
    float4 pos : SV_POSITION;
    float3 nrm : NORMAL;
};

VSOut vs_main(VSIn vin) {
    VSOut o;
    float4 world = mul(g_frame.object, float4(vin.pos, 1.0));
    o.pos = mul(g_frame.view_proj, world);
    o.nrm = mul((float3x3)g_frame.object, vin.nrm);
    return o;
}

float4 ps_main(VSOut pin) : SV_TARGET {
    float3 n = normalize(pin.nrm);
    float ndl = saturate(dot(n, normalize(float3(0.3, 0.8, 0.2))));
    return float4(ndl * float3(0.85, 0.88, 0.92), 1.0);
}
