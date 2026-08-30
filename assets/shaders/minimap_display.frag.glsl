#version 450

layout(set = 0, binding = 0) uniform sampler2D uComposite;

layout(push_constant) uniform Push {
    vec4 rect;
    vec2 playerUV;
    float rotation;
    float arrowRotation;
    float zoomRadius;
    int squareShape;
    float opacity;
} push;

layout(location = 0) in vec2 TexCoord;

layout(location = 0) out vec4 outColor;

float cross2d(vec2 a, vec2 b) {
    return a.x * b.y - a.y * b.x;
}

bool pointInTriangle(vec2 p, vec2 a, vec2 b, vec2 c) {
    float d1 = cross2d(b - a, p - a);
    float d2 = cross2d(c - b, p - b);
    float d3 = cross2d(a - c, p - c);
    bool hasNeg = (d1 < 0.0) || (d2 < 0.0) || (d3 < 0.0);
    bool hasPos = (d1 > 0.0) || (d2 > 0.0) || (d3 > 0.0);
    return !(hasNeg && hasPos);
}

void main() {
    vec2 center = TexCoord - 0.5;
    float dist = length(center);

    if (push.squareShape == 0) {
        if (dist > 0.5) discard;
    }

    float cs = cos(push.rotation);
    float sn = sin(push.rotation);
    vec2 mapCenter = vec2(-center.x, center.y);
    vec2 rotated = vec2(mapCenter.x * cs - mapCenter.y * sn, mapCenter.x * sn + mapCenter.y * cs);
    vec2 mapUV = push.playerUV + vec2(rotated.y, -rotated.x) * push.zoomRadius * 2.0;

    // Outside the composite is nothing, not more of the edge.
    //
    // The composite is three tiles square and the player stands somewhere in
    // the middle one, so the map has between one and one and a half tiles of
    // coverage in any direction - 533 to 800 yards. The furthest zoom level
    // asks for 800, which the coverage only meets when the player happens to
    // be centred in their tile. Every other position samples past the edge on
    // one side, and CLAMP_TO_EDGE answered with the last row stretched: a band
    // of vertical streaks along that side of the disc, made of real terrain
    // colours, which reads as corrupt rendering rather than as absent data.
    //
    // The same colour the missing-tile texture uses, so a hole in the map
    // looks the same wherever it comes from.
    vec4 mapColor;
    if (any(lessThan(mapUV, vec2(0.0))) || any(greaterThan(mapUV, vec2(1.0)))) {
        mapColor = vec4(12.0 / 255.0, 20.0 / 255.0, 30.0 / 255.0, 1.0);
    } else {
        mapColor = texture(uComposite, mapUV);
    }

    // Single player direction indicator (center arrow) rendered in-shader.
    vec2 local = center; // [-0.5, 0.5] around minimap center
    float ac = cos(push.arrowRotation);
    float as = sin(push.arrowRotation);
    // TexCoord Y grows downward on screen; use negative Y so 0-angle points North (up).
    vec2 tip   = vec2(0.0, -0.09);
    vec2 left  = vec2(-0.045, 0.02);
    vec2 right = vec2( 0.045, 0.02);
    mat2 rot = mat2(ac, -as, as, ac);
    tip = rot * tip;
    left = rot * left;
    right = rot * right;
    if (pointInTriangle(local, tip, left, right)) {
        mapColor.rgb = vec3(1.0, 0.86, 0.05);
    }
    float centerDot = smoothstep(0.016, 0.0, length(local));
    mapColor.rgb = mix(mapColor.rgb, vec3(1.0), centerDot * 0.95);

    // Dark border ring
    float border = smoothstep(0.48, 0.5, dist);
    if (push.squareShape == 0) {
        mapColor.rgb *= 1.0 - border * 0.7;
    }

    outColor = vec4(mapColor.rgb, mapColor.a * push.opacity);
}
