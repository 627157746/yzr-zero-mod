# -*- coding: utf-8 -*-
# Blender 预览渲染共用：给模型材质槽贴上底色贴图（来自 mod.json 的 textures 里 kind=BC 的条目）、建正交相机
import json, os
import bpy

def load_cfg(mod_dir):
    return json.load(open(os.path.join(mod_dir, 'mod.json'), encoding='utf-8'))

def apply_basecolor(cfg, mod_dir):
    done = []
    for t in cfg.get('textures', []):
        m = bpy.data.materials.get(t['slot'])
        if t['kind'] != 'BC' or m is None:   # 当前变体没有这个材质槽
            continue
        m.use_nodes = True; nt = m.node_tree
        bsdf = next((n for n in nt.nodes if n.type == 'BSDF_PRINCIPLED'), None) or nt.nodes.new('ShaderNodeBsdfPrincipled')
        img = nt.nodes.new('ShaderNodeTexImage'); img.image = bpy.data.images.load(os.path.join(mod_dir, t['source']))
        nt.links.new(img.outputs['Color'], bsdf.inputs['Base Color']); nt.nodes.active = img
        done.append(t['slot'])
    return done

def setup_scene(res_x, res_y, ortho):
    sc = bpy.context.scene
    sc.render.engine = 'BLENDER_WORKBENCH'
    sc.display.shading.light = 'STUDIO'; sc.display.shading.color_type = 'TEXTURE'
    sc.render.resolution_x, sc.render.resolution_y = res_x, res_y
    cd = bpy.data.cameras.new('cam'); cd.type = 'ORTHO'; cd.ortho_scale = ortho; cd.clip_end = 5000
    cam = bpy.data.objects.new('cam', cd); sc.collection.objects.link(cam); sc.camera = cam
    return sc, cam

def model_objects(cfg):
    return bpy.data.objects['Armature'], bpy.data.objects['SK_' + cfg['mod_name']]
