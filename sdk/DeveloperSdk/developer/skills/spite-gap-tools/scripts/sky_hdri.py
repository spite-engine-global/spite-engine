"""Blender sky -> equirect Radiance .hdr for importEnvironment (replaces HemisphereLight/gradient sky).
usage: blender -b --factory-startup -P sky_hdri.py -- <out.hdr> [width=1024] [sun_elevation_deg=15]
"""
import bpy, sys, math
a = sys.argv[sys.argv.index("--") + 1:]
out, width, elev = a[0], int(a[1]) if len(a) > 1 else 1024, float(a[2]) if len(a) > 2 else 15.0
bpy.ops.wm.read_factory_settings(use_empty=True)
s = bpy.context.scene
s.render.engine = 'CYCLES'; s.cycles.samples = 16
s.render.resolution_x, s.render.resolution_y = width, width // 2
w = bpy.data.worlds.new("sky"); s.world = w; w.use_nodes = True
sky = w.node_tree.nodes.new("ShaderNodeTexSky")  # sky_type: SINGLE_/MULTIPLE_SCATTERING, PREETHAM, HOSEK_WILKIE
if hasattr(sky, "sun_elevation"):
    sky.sun_elevation = math.radians(elev)
w.node_tree.links.new(sky.outputs[0], w.node_tree.nodes["Background"].inputs[0])
cam = bpy.data.cameras.new("c"); cam.type = 'PANO'; cam.panorama_type = 'EQUIRECTANGULAR'
o = bpy.data.objects.new("c", cam); s.collection.objects.link(o); s.camera = o
o.rotation_euler = (math.pi / 2, 0, 0)
s.render.image_settings.file_format = 'HDR'
s.render.filepath = out
bpy.ops.render.render(write_still=True)
