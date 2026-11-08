# -*- coding: utf-8 -*-
# Python 脚本共用：读取 soul.json + mods\<名字>\mod.json（mod.json 的 soul 段覆盖同名字段）
import json, os

def load(root, mod_dir):
    cfg = json.load(open(os.path.join(mod_dir, 'mod.json'), encoding='utf-8'))
    soul = json.load(open(os.path.join(root, 'soul.json'), encoding='utf-8'))
    soul.update(cfg.get('soul', {}))
    cfg['soul'] = soul
    return cfg
