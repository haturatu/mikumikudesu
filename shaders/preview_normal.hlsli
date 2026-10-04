// Sign must remain nonzero at the poles and octahedron seams.
float2 octSign(float2 v) {
    return float2(v.x >= 0.0 ? 1.0 : -1.0, v.y >= 0.0 ? 1.0 : -1.0);
}
float2 encodeOctNormal(float3 n) {
    n /= max(abs(n.x) + abs(n.y) + abs(n.z), 1e-6);
    return n.z >= 0.0 ? n.xy : (1.0 - abs(n.yx)) * octSign(n.xy);
}
float3 decodeOctNormal(float2 p) {
    float3 n = float3(p, 1.0 - abs(p.x) - abs(p.y));
    if (n.z < 0.0)
        n.xy = (1.0 - abs(n.yx)) * octSign(n.xy);
    return normalize(n);
}
