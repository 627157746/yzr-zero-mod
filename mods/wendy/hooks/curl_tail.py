# -*- coding: utf-8 -*-
# Wendy 专属：把尾巴 Tail_02..18 逐段上卷，烘焙进绑定姿势（rest pose）。
#
# 为什么要绑进 rest pose：
#   RigidBody 物理节点每帧把模拟刚体拉回「绑定姿势的相对朝向」（见 references/facts.md）。
#   所以游戏里尾巴在静止、走路、挥刀时都朝同一个绑定方向。要让「常态卷起、
#   运动时展开」，基底必须是卷的，运动时的展开靠物理惯性 + 重力把尾巴甩出去补足。
#
# 为什么不用 mod.json 的 rest_rotations：
#   该字段与 keep_shape_keys（Wendy 要保留 eye_blink 眨眼）互斥，会直接 fail。
#   所以走 hook：在 adapt_humanoid 之后、keep_morph.restore 之前改骨骼。
#
# 为什么这里的 apply 是安全的：
#   adapt_humanoid 第 2 步已经 shape_key_remove(all=True)，此刻网格没有形态键，
#   可以正常 Apply armature modifier；keep_morph.restore 在 hook 之后按新的
#   rest 矩阵重建 Blink，delta 会跟着新的绑定姿势转。
#
# 注入变量：bpy / arm / extras / K / CFG / ROOT / VARIANT / log / apply_all
import math
from mathutils import Matrix, Quaternion

ANGLE_DEG = -14.0   # 每段绕自身 X 轴的旋转；18 段累计约 -150°
FIRST = 2          # Tail_01 是根部，不转，避免整条尾巴从尾椎直接上翘

meshes = [o for o in bpy.data.objects if o.type == 'MESH']
turned = []

bpy.context.view_layer.objects.active = arm
bpy.ops.object.mode_set(mode='POSE')
for i in range(FIRST, 19):
    pb = arm.pose.bones.get(f'Tail_{i:02d}')
    if pb is None:
        continue
    pb.rotation_mode = 'QUATERNION'
    pb.rotation_quaternion = Quaternion((1.0, 0.0, 0.0), math.radians(ANGLE_DEG))
    turned.append(pb.name)
bpy.context.view_layer.update()
bpy.ops.object.mode_set(mode='OBJECT')
log('Wendy 尾巴上卷', len(turned), '段 ×', ANGLE_DEG, '度', turned[:3], '...', turned[-1:])

# 把姿势烘进网格，再把姿势烘成新的静止姿势（与 prepare_model.py 的 rest_rotations 同一路径）
for m in meshes:
    for md in [md for md in m.modifiers if md.type == 'ARMATURE']:
        bpy.context.view_layer.objects.active = m
        bpy.ops.object.modifier_apply(modifier=md.name)

bpy.context.view_layer.objects.active = arm
bpy.ops.object.mode_set(mode='POSE')
bpy.ops.pose.armature_apply(selected=False)
bpy.ops.object.mode_set(mode='OBJECT')

for m in meshes:
    md = m.modifiers.new('Armature', 'ARMATURE')
    md.object = arm

# 尾巴卷起来后整体抬高，重新落地一次，避免贴着地面或悬空
low = 0.0
sole = {'Left_Foot', 'Right_Foot', 'Left_Toes', 'Right_Toes'}
for m in meshes:
    gi = {g.index for g in m.vertex_groups if g.name in sole}
    for v in m.data.vertices:
        if any(g.group in gi and g.weight > 0.5 for g in v.groups):
            low = min(low, v.co.z)
if abs(low) > 1e-4:
    for o in bpy.data.objects:
        if o.type in ('ARMATURE', 'MESH'):
            o.matrix_world = Matrix.Translation((0, 0, -low)) @ o.matrix_world
    apply_all()
log('Wendy 尾巴卷起后鞋底修正(cm)', round(-low, 2))
