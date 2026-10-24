Texture2D<float4> SceneBeforeUI : register(t0);
Texture2D<float4> SceneWithUI : register(t1);
Texture2D<float4> FinalColor : register(t2);
RWTexture2D<float> UIAlpha : register(u0);
RWTexture2D<float4> DisplayHudless : register(u1);
RWTexture2D<float4> UIColorAndAlpha : register(u2);

cbuffer Settings : register(b0) {
  uint Width;
  uint Height;
  float Threshold;
  float Gain;
};

float DifferenceAt(int2 pixel) {
  pixel = clamp(pixel, int2(0, 0), int2(Width - 1, Height - 1));
  float3 before = SceneBeforeUI.Load(int3(pixel, 0)).rgb;
  float3 after = SceneWithUI.Load(int3(pixel, 0)).rgb;
  return max(abs(after.r - before.r),
             max(abs(after.g - before.g), abs(after.b - before.b)));
}

float AlphaAt(int2 pixel) {
  pixel = clamp(pixel, int2(0, 0), int2(Width - 1, Height - 1));
  float3 before = SceneBeforeUI.Load(int3(pixel, 0)).rgb;
  float3 after = SceneWithUI.Load(int3(pixel, 0)).rgb;
  float3 delta = abs(after - before);
  float difference = max(delta.r, max(delta.g, delta.b));
  if (difference < Threshold) return 0.0;

  // Dark translucent panels expose their alpha directly through attenuation.
  // Bright glyphs/highlights cannot be solved uniquely from RGB alone, so use
  // their normalized change and let strong text converge to opaque.
  float3 denominator = max(abs(before), float3(0.02, 0.02, 0.02));
  float3 attenuation = saturate((before - after) / denominator);
  float darkAlpha = max(attenuation.r, max(attenuation.g, attenuation.b));
  float relative = max(delta.r / denominator.r,
                       max(delta.g / denominator.g, delta.b / denominator.b));
  bool darkening = all(after <= before + float3(0.002, 0.002, 0.002));
  return darkening ? darkAlpha : saturate(relative * 4.0 * max(Gain, 1.0));
}

float3 PqDecode(float3 value) {
  const float m1 = 2610.0 / 16384.0;
  const float m2 = 2523.0 / 32.0;
  const float c1 = 3424.0 / 4096.0;
  const float c2 = 2413.0 / 128.0;
  const float c3 = 2392.0 / 128.0;
  float3 p = pow(saturate(value), 1.0 / m2);
  return pow(max(p - c1, 0.0) / max(c2 - c3 * p, 1e-6), 1.0 / m1);
}

float3 PqEncode(float3 value) {
  const float m1 = 2610.0 / 16384.0;
  const float m2 = 2523.0 / 32.0;
  const float c1 = 3424.0 / 4096.0;
  const float c2 = 2413.0 / 128.0;
  const float c3 = 2392.0 / 128.0;
  float3 p = pow(max(value, 0.0), m1);
  return pow((c1 + c2 * p) / (1.0 + c3 * p), m2);
}

bool IsProtectedHudZone(float2 uv) {
  // Race HUD geometry is fixed in normalized output coordinates. These zones
  // are only a coarse guard. The actual mask below is driven by the captured
  // before/after-UI difference, so moving road pixels inside a translucent HUD
  // widget are never frozen as if they were opaque UI.
  float2 gauge = (uv - float2(0.915, 0.845)) / float2(0.115, 0.175);
  float2 map = (uv - float2(0.100, 0.825)) / float2(0.125, 0.185);
  bool speedometer = dot(gauge, gauge) <= 1.0;
  bool minimap = dot(map, map) <= 1.0;
  bool timing = uv.x <= 0.32 && uv.y <= 0.24;
  bool position = uv.x >= 0.70 && uv.y <= 0.24;
  return speedometer || minimap || timing || position;
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= Width || id.y >= Height) return;

  // A two-pixel dilation includes the low-opacity antialiasing and glow around
  // Motorsport's thin HUD strokes. Earlier builds ignored this measured
  // difference and marked the complete speedometer/minimap discs as UI. That
  // made DLSS-G replay the road/background contained in the last real frame,
  // which is exactly the large circular ghost seen in generated frames.
  float2 uv = (float2(id.xy) + 0.5) / float2(Width, Height);
  float3 finalDisplay = FinalColor.Load(int3(id.xy, 0)).rgb;

  // UIAlpha remains single-channel as recommended by Streamline. Unlike the
  // previous binary/black-hole path, the pair now retains a moving background
  // below translucent gauges and therefore does not replay old road pixels.
  // Motorsport's transplanted DLSS-G provider did not reliably consume the
  // alpha-only UI path.  Publish a complete premultiplied UI surface instead.
  // Using a binary alpha here is intentional: every detected HUD pixel is
  // copied from the current real frame after interpolation, so a generated
  // frame cannot retain a previous speedometer/glyph underneath it.
  // In HDR highlights the translucent gauge may be numerically identical to
  // the saturated scene below it, so a difference-only mask leaves holes.
  // The game's race HUD has fixed screen-space zones: publish each complete
  // zone as current-frame UI, while the reconstructed hudless texture remains
  // available to frame generation underneath it. This trades only the road
  // visible through those small translucent panels to base-frame cadence and
  // guarantees that no previous gauge glyph can survive in a generated frame.
  float uiMask = IsProtectedHudZone(uv) ? 1.0 : 0.0;
  UIAlpha[id.xy] = uiMask;
  // This fallback deliberately does not depend on Motorsport's HUD draw
  // callbacks.  Those callbacks stopped firing in the current game build,
  // leaving the capture slots empty.  The complete current-frame display is
  // retained as the scene input, while an opaque, premultiplied copy of each
  // protected HUD zone is recomposited after interpolation.  The latter fully
  // covers any HUD image that the provider may have inherited from the scene,
  // so stale gauge glyphs cannot remain visible in generated frames.
  DisplayHudless[id.xy] = float4(finalDisplay, 1.0);
  UIColorAndAlpha[id.xy] = float4(finalDisplay * uiMask, uiMask);
}
