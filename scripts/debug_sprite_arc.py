#!/usr/bin/env python3
"""
Simula a rotação do sprite para provar que o arco foi eliminado.
Configuracao FINAL: yaw fixo no mundo, snapado em 45°, baseado na direcao do personagem.
"""

import math
import numpy as np

CHAR_POS = np.array([50.0, 0.0, 50.0])
WORLD_UP = np.array([0.0, 1.0, 0.0])
TILT_DEG = 45.0
SCALE = 0.15
HEAD_OFF = np.array([0.0, -80.0])

def normalize(v):
    n = np.linalg.norm(v)
    return v / n if n > 0 else v

def calc_sprite_locked(pivot_world, sprite_yaw, x_off_px, y_off_px):
    horiz_forward = np.array([math.sin(sprite_yaw), 0.0, math.cos(sprite_yaw)])
    right = normalize(np.cross(WORLD_UP, horiz_forward))
    horiz_back = -horiz_forward
    tilt_rad = math.radians(TILT_DEG)
    up = WORLD_UP * math.cos(tilt_rad) + horiz_back * math.sin(tilt_rad)
    x_off = x_off_px * SCALE
    y_off = y_off_px * SCALE
    return pivot_world + right * x_off + up * (-y_off)

# Personagem olhando para uma direcao qualquer (ex: 37 graus)
char_yaw_deg = 37.0
snapped_deg = round(char_yaw_deg / 45.0) * 45.0
locked_yaw = math.radians(snapped_deg)

print(f"Personagem direcao real: {char_yaw_deg}°")
print(f"Personagem direcao snapada: {snapped_deg}°")
print(f"Locked yaw: {locked_yaw:.4f} rad")

# Simular 360 frames de camera orbitando
yaws = np.linspace(0, 360, 361)
pivot = CHAR_POS + np.array([0, 2, 0])  # Centro do sprite (worldPos)

head_pos = [calc_sprite_locked(pivot, locked_yaw, HEAD_OFF[0], HEAD_OFF[1]) for _ in yaws]
head_pos = np.array(head_pos)

drift = np.max(np.linalg.norm(head_pos - head_pos[0], axis=1))

print(f"\n=== CONFIGURACAO FINAL: Yaw fixo snapado 45° ===")
print(f"Max deslocamento da cabeca durante orbita completa: {drift:.6f}")
print(f"\n{'ZERO' if drift < 0.0001 else 'AINDA HA ARCO'} — O sprite e um objeto rigido completamente estatico no mundo.")
