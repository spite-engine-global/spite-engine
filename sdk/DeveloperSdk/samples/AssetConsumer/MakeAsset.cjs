const fs = require("node:fs");
const path = require("node:path");
const { deflateSync, crc32 } = require("node:zlib");

// Generate a tileable two-color checker texture without any external inputs.
function uint32(value) {
    const bytes = Buffer.alloc(4);
    bytes.writeUInt32BE(value);
    return bytes;
}

function pngChunk(type, data) {
    const payload = Buffer.concat([Buffer.from(type), data]);
    return Buffer.concat([uint32(data.length), payload, uint32(crc32(payload))]);
}

const seed = Number(process.env.SPITE_ASSET_SEED ?? "731");
if (!Number.isInteger(seed) || seed < 0 || seed > 0xffffffff)
    throw new Error("SPITE_ASSET_SEED must be an unsigned 32-bit integer");
const size = 64;
const pngHeader = Buffer.concat([uint32(size), uint32(size), Buffer.from([8, 2, 0, 0, 0])]);
const scanlines = Buffer.alloc(size * (1 + 3 * size));
for (let y = 0; y < size; y++) {
    for (let x = 0; x < size; x++) {
        const color = (Math.floor(x / 8) + Math.floor(y / 8)) % 2 === 0
            ? [18, 166, 173] : [249, 162, 59];
        const noise = (Math.imul(x + 1, 73856093) ^ Math.imul(y + 1, 19349663) ^ seed) >>> 0;
        const wear = noise % 13;
        scanlines.set(color.map(channel => Math.max(0, channel - wear)), y * (1 + 3 * size) + 1 + x * 3);
    }
}
const png = Buffer.concat([
    Buffer.from("89504e470d0a1a0a", "hex"),
    pngChunk("IHDR", pngHeader),
    pngChunk("IDAT", deflateSync(scanlines)),
    pngChunk("IEND", Buffer.alloc(0))
]);

// Construct a faceted hexagonal bollard with UVs on the sides and caps.
const positions = [], normals = [], uv = [], indices = [];
const radius = 0.65, height = 1.5;
const ring = Array.from({ length: 6 }, (_, side) => {
    const angle = side * Math.PI / 3;
    return [radius * Math.cos(angle), radius * Math.sin(angle)];
});
function addVertex(x, y, z, normal, texcoord) {
    const index = positions.length / 3;
    positions.push(x, y, z);
    normals.push(...normal);
    uv.push(...texcoord);
    return index;
}
for (let side = 0; side < ring.length; side++) {
    const [ax, az] = ring[side];
    const [bx, bz] = ring[(side + 1) % ring.length];
    const angle = (side + 0.5) * Math.PI / 3;
    const normal = [Math.cos(angle), 0, Math.sin(angle)];
    const first = addVertex(ax, 0, az, normal, [0, 1]);
    addVertex(ax, height, az, normal, [0, 0]);
    addVertex(bx, height, bz, normal, [1, 0]);
    addVertex(bx, 0, bz, normal, [1, 1]);
    indices.push(first, first + 1, first + 2, first, first + 2, first + 3);
}
for (const [y, sign] of [[0, -1], [height, 1]]) {
    const normal = [0, sign, 0];
    const center = addVertex(0, y, 0, normal, [0.5, 0.5]);
    for (const [x, z] of ring) addVertex(x, y, z, normal, [x / (2 * radius) + 0.5, z / (2 * radius) + 0.5]);
    for (let side = 0; side < ring.length; side++) {
        const a = center + 1 + side, b = center + 1 + (side + 1) % ring.length;
        indices.push(center, ...(sign === 1 ? [b, a] : [a, b]));
    }
}

function binaryArray(values, width, method) {
    const result = Buffer.alloc(values.length * width);
    values.forEach((value, index) => result[method](value, index * width));
    return result;
}
const segments = [
    binaryArray(positions, 4, "writeFloatLE"),
    binaryArray(normals, 4, "writeFloatLE"),
    binaryArray(uv, 4, "writeFloatLE"),
    binaryArray(indices, 2, "writeUInt16LE"),
    png
];
function aligned(bytes, pad = 0) {
    return Buffer.concat([bytes, Buffer.alloc((4 - bytes.length % 4) % 4, pad)]);
}
let byteOffset = 0;
const bufferViews = segments.map((segment, index) => {
    const view = { buffer: 0, byteOffset, byteLength: segment.length };
    if (index < 4) view.target = index === 3 ? 34963 : 34962;
    byteOffset += aligned(segment).length;
    return view;
});
const bytes = Buffer.concat(segments.map(segment => aligned(segment)));
const extrema = [0, 1, 2].map(component => positions.filter((_, index) => index % 3 === component).map(Math.fround));
const gltf = {
    asset: { version: "2.0", generator: "SPITE SDK asset consumer" },
    scene: 0, scenes: [{ nodes: [0] }], nodes: [{ mesh: 0, name: "Checker bollard", extras: { spite_source_id: "checker-bollard" } }],
    meshes: [{ primitives: [{ attributes: { POSITION: 0, NORMAL: 1, TEXCOORD_0: 2 }, indices: 3, material: 0 }] }],
    materials: [{ name: "Amber and teal checks", pbrMetallicRoughness: {
        baseColorTexture: { index: 0 }, metallicFactor: 0, roughnessFactor: 0.7
    } }],
    textures: [{ source: 0 }], images: [{ mimeType: "image/png", bufferView: 4 }],
    buffers: [{ byteLength: bytes.length }], bufferViews,
    accessors: [
        { bufferView: 0, componentType: 5126, count: positions.length / 3, type: "VEC3",
            min: extrema.map(values => Math.min(...values)), max: extrema.map(values => Math.max(...values)) },
        { bufferView: 1, componentType: 5126, count: normals.length / 3, type: "VEC3" },
        { bufferView: 2, componentType: 5126, count: uv.length / 2, type: "VEC2" },
        { bufferView: 3, componentType: 5123, count: indices.length, type: "SCALAR" }
    ]
};
function glbChunk(type, data) {
    const header = Buffer.alloc(8);
    header.writeUInt32LE(data.length);
    header.write(type, 4, "ascii");
    return Buffer.concat([header, data]);
}
const chunks = Buffer.concat([
    glbChunk("JSON", aligned(Buffer.from(JSON.stringify(gltf)), 0x20)),
    glbChunk("BIN\0", bytes)
]);
const header = Buffer.alloc(12);
header.write("glTF");
header.writeUInt32LE(2, 4);
header.writeUInt32LE(header.length + chunks.length, 8);
if (!process.env.SPITE_ASSET_OUTPUT) throw new Error("Set SPITE_ASSET_OUTPUT to a writable output directory");
const directory = path.resolve(process.env.SPITE_ASSET_OUTPUT);
fs.mkdirSync(directory, { recursive: true });
const output = path.join(directory, "sdk_checker_bollard.glb");
fs.writeFileSync(output, Buffer.concat([header, chunks]));
console.log(JSON.stringify({ output, vertices: positions.length / 3, triangles: indices.length / 3, texture: [size, size] }));
