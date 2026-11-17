const fs = require("node:fs");

// Validates the tightly packed attributes emitted by MakeAsset, not arbitrary glTF.
function validate(file) {
    const glb = fs.readFileSync(file);
    if (glb.toString("ascii", 0, 4) !== "glTF" || glb.readUInt32LE(4) !== 2 || glb.readUInt32LE(8) !== glb.length)
        throw new Error("Invalid GLB header or length");
    const jsonLength = glb.readUInt32LE(12);
    if (glb.toString("ascii", 16, 20) !== "JSON") throw new Error("Missing JSON chunk");
    const doc = JSON.parse(glb.toString("utf8", 20, 20 + jsonLength));
    const binHeader = 20 + jsonLength;
    if (glb.toString("ascii", binHeader + 4, binHeader + 8) !== "BIN\0" ||
        binHeader + 8 + glb.readUInt32LE(binHeader) !== glb.length) throw new Error("Invalid BIN chunk");
    const bin = glb.subarray(binHeader + 8);
    const primitive = doc.meshes[0].primitives[0];
    function read(index, count, componentType) {
        const a = doc.accessors[index], view = doc.bufferViews[a.bufferView];
        const width = componentType === 5126 ? 4 : 2;
        if (a.componentType !== componentType || a.type !== ({ 1: "SCALAR", 2: "VEC2", 3: "VEC3" })[count] ||
            view.buffer !== 0 || view.byteStride || a.sparse ||
            (a.byteOffset || 0) + a.count * count * width > view.byteLength || view.byteOffset + view.byteLength > bin.length)
            throw new Error("Unsupported or out-of-range accessor");
        return Array.from({ length: a.count * count }, (_, i) => {
            const offset = view.byteOffset + (a.byteOffset || 0) + i * width;
            return componentType === 5126 ? bin.readFloatLE(offset) : bin.readUInt16LE(offset);
        });
    }
    const pos = read(primitive.attributes.POSITION, 3, 5126);
    const normals = read(primitive.attributes.NORMAL, 3, 5126);
    const uv = read(primitive.attributes.TEXCOORD_0, 2, 5126);
    const indices = read(primitive.indices, 1, 5123);
    const vertices = pos.length / 3;
    if (!vertices || normals.length !== pos.length || uv.length !== vertices * 2 || !indices.length || indices.length % 3 ||
        [...pos, ...normals, ...uv].some(v => !Number.isFinite(v)) || indices.some(i => i >= vertices))
        throw new Error("Invalid attribute counts, values or indices");
    for (let i = 0; i < vertices; i++) {
        const n = normals.slice(i * 3, i * 3 + 3);
        if (Math.abs(Math.hypot(...n) - 1) > 1e-4) throw new Error("Normal is not unit length");
        if (uv[i * 2] < 0 || uv[i * 2] > 1 || uv[i * 2 + 1] < 0 || uv[i * 2 + 1] > 1)
            throw new Error("UV outside the sample atlas");
    }
    for (let i = 0; i < indices.length; i += 3) {
        const [a, b, c] = indices.slice(i, i + 3).map(index => pos.slice(index * 3, index * 3 + 3));
        const u = b.map((v, j) => v - a[j]), v = c.map((v, j) => v - a[j]);
        const cross = [u[1]*v[2]-u[2]*v[1], u[2]*v[0]-u[0]*v[2], u[0]*v[1]-u[1]*v[0]];
        if (Math.hypot(...cross) < 1e-8) throw new Error("Degenerate triangle");
        for (const index of indices.slice(i, i + 3)) {
            const n = normals.slice(index * 3, index * 3 + 3);
            if (cross.reduce((sum, x, j) => sum + x * n[j], 0) <= 0) throw new Error("Triangle winding disagrees with normal");
        }
    }
    if (doc.nodes[0]?.extras?.spite_source_id !== "checker-bollard") throw new Error("Missing stable source ID");
    return { vertices, triangles: indices.length / 3, sourceId: doc.nodes[0].extras.spite_source_id };
}
if (require.main === module) console.log(JSON.stringify(validate(process.argv[2])));
module.exports = { validate };
