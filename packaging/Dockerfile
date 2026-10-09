# The VoxelSieve image: the installed tree of the Linux package on Ubuntu 24.04, the system the
# package is built on. The build context is the directory holding the unpacked package, as the
# release workflow does it (.github/workflows/release.yml):
#
#   docker build -f packaging/Dockerfile --build-arg PACKAGE=voxelsieve-<version>-linux-x64 <dir>
#
# By default it serves the browser UI on port 8410 for the data mounted at /data; with --mcp it
# serves MCP on stdin/stdout (docker run -i). See docs/quickstart.md.
FROM ubuntu:24.04

ARG PACKAGE
ARG VERSION=unknown
LABEL org.opencontainers.image.title="VoxelSieve" \
      org.opencontainers.image.description="Industrial CT analysis on OpenVDB: porosity, surfaces, nominal-actual comparison and reports, as browser UI and MCP server" \
      org.opencontainers.image.source="https://github.com/yoshi5534/VoxelSieve" \
      org.opencontainers.image.licenses="Apache-2.0" \
      org.opencontainers.image.version="${VERSION}"

COPY ${PACKAGE}/ /opt/voxelsieve/
# Analyses run as an unprivileged user; give the host user's id with --user to write to a mounted
# directory it owns.
RUN useradd --system --create-home --uid 10001 voxelsieve \
 && mkdir -p /data && chown voxelsieve /data
USER voxelsieve
ENV PATH=/opt/voxelsieve/bin:$PATH
WORKDIR /data
VOLUME /data
EXPOSE 8410
ENTRYPOINT ["vs-studio"]
CMD ["--host", "0.0.0.0", "/data"]
