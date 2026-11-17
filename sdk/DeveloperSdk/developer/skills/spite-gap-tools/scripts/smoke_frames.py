"""Mantaflow smoke or fire -> numbered RGBA PNG frames for a flipbook atlas (then atlas.py).
usage: blender -b --factory-startup -P smoke_frames.py -- <out_dir> [frames=16] [size=128] [fire=0]
"""
import bpy, sys, os
a = sys.argv[sys.argv.index("--") + 1:]
out, frames, size, fire = a[0], int(a[1]) if len(a) > 1 else 16, int(a[2]) if len(a) > 2 else 128, a[3:4] == ["1"]
bpy.ops.wm.read_factory_settings(use_empty=True)
s = bpy.context.scene
bpy.ops.mesh.primitive_uv_sphere_add(radius=0.3, location=(0, 0, 0))
emitter = bpy.context.object
bpy.ops.object.quick_smoke(style='BOTH' if fire else 'SMOKE')
emitter.visible_camera = emitter.visible_shadow = False  # hide_render would stop the emission
domain = next(o for o in s.objects if o.modifiers.get("Fluid") and o.modifiers["Fluid"].fluid_type == 'DOMAIN')
d = domain.modifiers["Fluid"].domain_settings
warmup = 10
s.frame_start, s.frame_end = 1, warmup + frames * 2
d.resolution_max = 64
d.cache_type = 'ALL'
d.cache_directory = os.path.join(out, "_cache")
d.cache_frame_start, d.cache_frame_end = s.frame_start, s.frame_end
bpy.context.view_layer.objects.active = domain
with bpy.context.temp_override(object=domain, active_object=domain):
    bpy.ops.fluid.bake_all()
s.render.engine = 'CYCLES'; s.cycles.samples = 32
s.render.film_transparent = True
s.render.resolution_x = s.render.resolution_y = size
s.render.image_settings.file_format = 'PNG'; s.render.image_settings.color_mode = 'RGBA'
cam = bpy.data.objects.new("cam", bpy.data.cameras.new("cam")); s.collection.objects.link(cam); s.camera = cam
cam.data.type = 'ORTHO'; cam.data.ortho_scale = max(domain.dimensions.x, domain.dimensions.z)
cam.location = (domain.location.x, domain.location.y - 10, domain.location.z); cam.rotation_euler = (1.5708, 0, 0)
sun = bpy.data.objects.new("sun", bpy.data.lights.new("sun", 'SUN')); s.collection.objects.link(sun)
sun.data.energy = 3; sun.rotation_euler = (0.8, 0.2, 0.5)
s.frame_start, s.frame_step = warmup + 1, 2
s.render.filepath = os.path.join(out, "f_")
bpy.ops.render.render(animation=True)
