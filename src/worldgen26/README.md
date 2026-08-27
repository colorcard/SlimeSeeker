# Minecraft 26.2 worldgen provenance

The files under `third_party/cubiomes/` are a whitespace-normalized subset of Cubiomes
commit `e61f90580cbdd883214a8054670dacae655e59c0` (2024-11-10), licensed under
the MIT license in `licenses/Cubiomes.txt`. The obsolete built-in biome mapping
tables are omitted; only climate noise, terrain depth, SHA-256 biome zoom, and
their support code are compiled.

`generated/biome_parameters_26_2.inc` contains the 7,594 parameter points
emitted by Minecraft 26.2 `OverworldBiomeBuilder.addBiomes`. Biome identifiers
were reduced to the MONSTER spawn-weight profiles used by the second-stage
score. The table is factual generated data; Java, Fabric Loom, and the research
export are not runtime or build dependencies.

The climate noise constants and terrain depth path were checked against the
unpacked 26.2 sources. Golden coordinates in `tests/unit/worldgen26_tests.cpp`
were sampled through the original 26.2 `RandomState`, `MultiNoiseBiomeSource`,
and `BiomeManager`, including the new sulfur caves profile.
