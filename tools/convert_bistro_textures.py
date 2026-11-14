#!/usr/bin/env python3
"""
tools/convert_bistro_textures.py

Converts checked-out Amazon Lumberyard Bistro DDS textures to standard PNG format
at the exact relative paths expected by bistro.gltf, applying optimal downscaling
(max 2048x2048 for hero assets / foliage, max 1024x1024 for tiling surfaces) to
balance supreme visual fidelity, fast streaming, and reasonable VRAM footprint.
"""

import os
import sys
import json
from PIL import Image

def main():
    base_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    bistro_dir = os.path.join(base_dir, "test-assets", "benchmark", "bistro")
    gltf_path = os.path.join(bistro_dir, "bistro.gltf")
    src_git_root = "/tmp/bistro_git"

    if not os.path.isfile(gltf_path):
        print(f"[ERROR] bistro.gltf not found at {gltf_path}")
        sys.exit(1)

    with open(gltf_path, "r", encoding="utf-8") as f:
        gltf_data = json.load(f)

    textures = gltf_data.get("textures", [])
    images = gltf_data.get("images", [])

    needed_textures = []
    for m in gltf_data.get("materials", []):
        ext = m.get("extensions", {}).get("KHR_materials_pbrSpecularGlossiness", {})
        diff_tex = ext.get("diffuseTexture", {}).get("index")
        if diff_tex is None:
            pbr = m.get("pbrMetallicRoughness", {})
            diff_tex = pbr.get("baseColorTexture", {}).get("index")

        if diff_tex is not None and diff_tex < len(textures):
            t = textures[diff_tex]
            png_idx = t.get("source")
            dds_idx = t.get("extensions", {}).get("MSFT_texture_dds", {}).get("source")
            png_uri = images[png_idx]["uri"] if png_idx is not None and png_idx < len(images) else None
            dds_uri = images[dds_idx]["uri"] if dds_idx is not None and dds_idx < len(images) else None
            if png_uri:
                needed_textures.append((m.get("name", ""), png_uri, dds_uri))

    print(f"Total material texture mappings found in glTF: {len(needed_textures)}")

    converted_count = 0
    already_present = 0
    not_found = 0

    for mat_name, png_uri, dds_uri in needed_textures:
        dst_png_path = os.path.join(bistro_dir, png_uri)
        if os.path.isfile(dst_png_path) and os.path.getsize(dst_png_path) > 0:
            already_present += 1
            continue

        # Look for source DDS
        src_dds_path = None
        candidates = []
        if dds_uri:
            candidates.append(os.path.join(src_git_root, dds_uri))
        # Also try replacing .png with .dds
        dds_alt = os.path.splitext(png_uri)[0] + ".dds"
        candidates.append(os.path.join(src_git_root, dds_alt))
        # Case insensitive filename fallback
        base_name = os.path.basename(dds_alt)
        parent_dir = os.path.join(src_git_root, os.path.dirname(dds_alt))
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

        # Convert DDS to PNG
        try:
            os.makedirs(os.path.dirname(dst_png_path), exist_ok=True)
            img = Image.open(src_dds_path)
            # Max dimension: 2048 for hero/facade/vegetation, 1024 for smaller tiling
            max_dim = 2048
            if "tiling" in png_uri or "small" in png_uri:
                max_dim = 1024
            
            if img.size[0] > max_dim or img.size[1] > max_dim:
                scale = max_dim / max(img.size)
                new_size = (int(img.size[0] * scale), int(img.size[1] * scale))
                img = img.resize(new_size, Image.Resampling.BILINEAR)

            img.save(dst_png_path)
            converted_count += 1
            size_kb = os.path.getsize(dst_png_path) / 1024
            print(f"[CONVERTED] {mat_name}: {png_uri} ({img.size[0]}x{img.size[1]}, {size_kb:.1f} KB)")
        except Exception as e:
            print(f"[ERROR] Failed to convert {src_dds_path}: {e}")

    print("\n=== Bistro Texture Ingestion Summary ===")
    print(f"Already Present: {already_present}")
    print(f"Newly Converted: {converted_count}")
    print(f"Missing from Git Checkout: {not_found}")
    print("========================================")

if __name__ == "__main__":
    main()
