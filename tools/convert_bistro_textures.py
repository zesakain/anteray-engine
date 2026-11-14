#!/usr/bin/env python3
"""
tools/convert_bistro_textures.py

Converts checked-out Amazon Lumberyard Bistro DDS textures to standard PNG format
at the exact relative paths expected by bistro.gltf:
- Diffuse / BaseColor textures (sRGB, preserving alpha for cutout foliage / glass)
- Normal maps (_ddna.dds -> standard tangent-space normal maps _ddna.png)
- Specular-Glossiness maps (_spec.dds converted to glTF standard Metallic-Roughness _spec.png)
- Emissive maps (_emi.dds / _em.dds)

Applies optimal downscaling (max 2048x2048 for hero assets / foliage, max 1024x1024
for tiling surfaces) to balance supreme visual fidelity, fast streaming, and reasonable VRAM footprint.
"""

import os
import sys
import json
import numpy as np
from PIL import Image


def convert_normal_map(img: Image.Image) -> Image.Image:
    """Ensure valid tangent-space normal map (RGB with valid Z pointing outward)."""
    arr = np.array(img)
    if arr.ndim == 2:
        arr = np.stack([arr, arr, arr], axis=-1)
    
    # Extract RGB
    rgb = arr[:, :, :3].astype(np.float32) / 255.0
    
    # Normal X, Y in [-1, 1]
    nx = rgb[:, :, 0] * 2.0 - 1.0
    ny = rgb[:, :, 1] * 2.0 - 1.0
    
    # If B is essentially zero (BC5 2-channel normal map), reconstruct Z
    if rgb[:, :, 2].max() < 0.1:
        nz = np.sqrt(np.clip(1.0 - nx * nx - ny * ny, 0.0, 1.0))
        rgb[:, :, 2] = nz
    
    # Re-normalize to [0, 255]
    out_arr = np.clip(rgb * 255.0, 0.0, 255.0).astype(np.uint8)
    return Image.fromarray(out_arr, mode="RGB")


def convert_spec_gloss_to_metallic_roughness(img: Image.Image) -> Image.Image:
    """
    Mathematically convert KHR_materials_pbrSpecularGlossiness texture to
    standard glTF 2.0 Metallic-Roughness layout:
      - Channel R: Occlusion (255)
      - Channel G: Roughness = clamp(1.0 - Glossiness, 0.045, 1.0)
      - Channel B: Metallic = clamp((max(R, G, B) - 0.04) / 0.96, 0.0, 1.0)
      - Channel A: 255
    """
    arr = np.array(img).astype(np.float32) / 255.0
    h, w = arr.shape[:2]
    
    # Specular color RGB
    if arr.ndim == 2:
        r = g = b = arr
        gloss = np.ones((h, w), dtype=np.float32)
    elif arr.shape[2] >= 3:
        r = arr[:, :, 0]
        g = arr[:, :, 1]
        b = arr[:, :, 2]
        gloss = arr[:, :, 3] if arr.shape[2] >= 4 else np.ones((h, w), dtype=np.float32)
    else:
        r = g = b = arr[:, :, 0]
        gloss = np.ones((h, w), dtype=np.float32)
    
    # Analytical conversion
    roughness = np.clip(1.0 - gloss, 0.045, 1.0)
    spec_max = np.maximum(r, np.maximum(g, b))
    
    # Dielectric threshold F0 = 0.04
    metallic = np.where(spec_max <= 0.05, 0.0, np.clip((spec_max - 0.04) / 0.96, 0.0, 1.0))
    
    out_arr = np.zeros((h, w, 4), dtype=np.uint8)
    out_arr[:, :, 0] = 255                                 # R: Occlusion
    out_arr[:, :, 1] = (roughness * 255.0).astype(np.uint8) # G: Roughness
    out_arr[:, :, 2] = (metallic * 255.0).astype(np.uint8)  # B: Metallic
    out_arr[:, :, 3] = 255                                 # A: 255
    
    return Image.fromarray(out_arr, mode="RGBA")


def main():
    base_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    bistro_dir = os.path.join(base_dir, "test-assets", "benchmark", "bistro")
    gltf_path = os.path.join(bistro_dir, "bistro.gltf")
    src_git_roots = ["/tmp/bistro_git", "/tmp/bistro_hero"]

    if not os.path.isfile(gltf_path):
        print(f"[ERROR] bistro.gltf not found at {gltf_path}")
        sys.exit(1)

    with open(gltf_path, "r", encoding="utf-8") as f:
        gltf_data = json.load(f)

    textures = gltf_data.get("textures", [])
    images = gltf_data.get("images", [])

    needed_textures = {}
    for m in gltf_data.get("materials", []):
        mat_name = m.get("name", "")
        pbr = m.get("pbrMetallicRoughness", {})
        sg = m.get("extensions", {}).get("KHR_materials_pbrSpecularGlossiness", {})

        diff_idx = sg.get("diffuseTexture", {}).get("index", pbr.get("baseColorTexture", {}).get("index"))
        norm_idx = m.get("normalTexture", {}).get("index")
        spec_idx = sg.get("specularGlossinessTexture", {}).get("index", pbr.get("metallicRoughnessTexture", {}).get("index"))
        emiss_idx = m.get("emissiveTexture", {}).get("index")

        slots = [("diff", diff_idx), ("norm", norm_idx), ("spec", spec_idx), ("emiss", emiss_idx)]
        for slot_type, t_idx in slots:
            if t_idx is not None and t_idx < len(textures):
                t = textures[t_idx]
                png_idx = t.get("source")
                dds_idx = t.get("extensions", {}).get("MSFT_texture_dds", {}).get("source")
                png_uri = images[png_idx]["uri"] if png_idx is not None and png_idx < len(images) else None
                dds_uri = images[dds_idx]["uri"] if dds_idx is not None and dds_idx < len(images) else None
                if png_uri and png_uri not in needed_textures:
                    needed_textures[png_uri] = (mat_name, slot_type, dds_uri)

    print(f"Total unique material texture mappings in glTF: {len(needed_textures)}")

    converted_count = 0
    already_present = 0
    not_found = 0

    for png_uri, (mat_name, slot_type, dds_uri) in needed_textures.items():
        dst_png_path = os.path.join(bistro_dir, png_uri)
        if os.path.isfile(dst_png_path) and os.path.getsize(dst_png_path) > 0:
            already_present += 1
            continue

        # Look for source DDS
        src_dds_path = None
        candidates = []
        for root in src_git_roots:
            if dds_uri:
                candidates.append(os.path.join(root, dds_uri))
            dds_alt = os.path.splitext(png_uri)[0] + ".dds"
            candidates.append(os.path.join(root, dds_alt))

            # Case insensitive filename fallback
            base_name = os.path.basename(dds_alt)
            parent_dir = os.path.join(root, os.path.dirname(dds_alt))
            if os.path.isdir(parent_dir):
                for f in os.listdir(parent_dir):
                    if f.lower() == base_name.lower():
                        candidates.append(os.path.join(parent_dir, f))

        for cand in candidates:
            if os.path.isfile(cand) and os.path.getsize(cand) > 0:
                src_dds_path = cand
                break

        if not src_dds_path:
            not_found += 1
            continue

        # Convert DDS to PNG according to slot
        try:
            os.makedirs(os.path.dirname(dst_png_path), exist_ok=True)
            raw_img = Image.open(src_dds_path)
            
            # Max dimension: 2048 for hero/facade/vegetation, 1024 for smaller tiling
            max_dim = 2048
            if "tiling" in png_uri or "small" in png_uri:
                max_dim = 1024

            if slot_type == "norm":
                img = convert_normal_map(raw_img)
            elif slot_type == "spec":
                img = convert_spec_gloss_to_metallic_roughness(raw_img)
            else:
                img = raw_img

            if img.size[0] > max_dim or img.size[1] > max_dim:
                scale = max_dim / max(img.size)
                new_size = (int(img.size[0] * scale), int(img.size[1] * scale))
                img = img.resize(new_size, Image.Resampling.BILINEAR)

            img.save(dst_png_path)
            converted_count += 1
            size_kb = os.path.getsize(dst_png_path) / 1024
            print(f"[CONVERTED] [{slot_type.upper()}] {mat_name}: {png_uri} ({img.size[0]}x{img.size[1]}, {size_kb:.1f} KB)")
        except Exception as e:
            print(f"[ERROR] Failed to convert {src_dds_path}: {e}")

    print("\n=== Bistro Texture Ingestion Summary ===")
    print(f"Already Present: {already_present}")
    print(f"Newly Converted: {converted_count}")
    print(f"Missing from Git Checkout: {not_found}")
    print("========================================")


if __name__ == "__main__":
    main()
