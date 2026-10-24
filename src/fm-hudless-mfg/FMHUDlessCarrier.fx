// A deliberately invisible ReShade technique.
//
// The Motorsport HUDless add-on records its UI-alpha compute pass from the
// reshade_render_technique callback, where ReShade guarantees that the D3D12
// command list is open and owned by its normal submission path.  ReShade does
// not create that command list when no effect technique is enabled, so this
// one-thread no-op compute pass keeps the callback alive without touching the
// backbuffer.

[shader("compute")]
void FMHUDlessCarrierCS(uint3 id : SV_DispatchThreadID)
{
    // Keep the dispatch observable to the effect runtime while producing no
    // resources and no visible output.
    if (id.x == 0xFFFFFFFF)
        return;
}

technique FMHUDlessCarrier
<
    enabled = true;
    hidden = true;
>
{
    pass
    {
        ComputeShader = FMHUDlessCarrierCS<1, 1, 1>;
        DispatchSizeX = 1;
        DispatchSizeY = 1;
        DispatchSizeZ = 1;
    }
}
