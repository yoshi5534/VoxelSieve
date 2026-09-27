# VoxelSieve

Open-source tooling for industrial CT volumes, built on [OpenVDB](https://www.openvdb.org/).

Reconstructed CT volumes are large, uncompressed raw files in which most voxels are air.
VoxelSieve separates the surrounding air from the part, keeps internal voids such as pores,
and stores the result as a sparse VDB grid that can be rendered quickly and analysed further.

Status: early development.

## License

Apache-2.0, see [LICENSE](LICENSE).
