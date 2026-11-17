"""3D text -> standing GLB mesh (three.js TextGeometry).
usage: blender -b --factory-startup -P text_mesh.py -- <out.glb> "<text>" [hex_color=ff4d2e] [extrude=0.1]
"""
import bpy, sys, math
a = sys.argv[sys.argv.index("--") + 1:]
out, body = a[0], a[1]
color = a[2] if len(a) > 2 else "ff4d2e"; extrude = float(a[3]) if len(a) > 3 else 0.1
bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.object.text_add()
t = bpy.context.object
t.data.body = body; t.data.extrude = extrude; t.data.align_x = 'CENTER'
t.rotation_euler[0] = math.pi / 2  # Blender Z-up: without this the text lies flat in SPITE
bpy.ops.object.convert(target='MESH')
bpy.ops.object.transform_apply(rotation=True)
m = bpy.data.materials.new("text"); m.use_nodes = True
rgb = [int(color[i:i + 2], 16) / 255 for i in (0, 2, 4)]
m.node_tree.nodes["Principled BSDF"].inputs["Base Color"].default_value = (*[c ** 2.2 for c in rgb], 1)
t.data.materials.append(m)
bpy.ops.export_scene.gltf(filepath=out, export_format='GLB')
