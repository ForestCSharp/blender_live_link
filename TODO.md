1. **Tangent-space normal mapping**  
   Add Blender normal-map export, mesh tangents, and TBN reconstruction. This would dramatically improve surface detail on metal, stone, fabric, and mech panels without requiring additional geometry.

2. **Hybrid screen-space reflections with parallax-corrected probe fallback**  
   Use screen-space reflections for visible nearby geometry and box-projected GI probes when rays leave the screen or miss. Roughness-aware tracing and temporal accumulation would produce more convincing reflective floors, walls, and metallic objects.

