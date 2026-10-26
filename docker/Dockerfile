# Build image shared by build.ps1, the devcontainer and GitHub Actions.
#
# Base: zmkfirmware/zmk-build-arm:4.4-branch (2026-06-12), Zephyr SDK 1.0.1,
# which Zephyr 4.5 expects. ZMK publishes no 4.5 image yet.
FROM zmkfirmware/zmk-build-arm@sha256:f03afad9fa0595893af3511c382104b32724cdde34aaa562574f0bc922e49f9d

# The base image ships Zephyr 4.4's Python requirements and removes pip.
# Must match the zephyr revision in config/west.yml.
ARG ZEPHYR_REV=8597b2fa1b965f121ec507916b182cb0c60eb4a6
ADD https://raw.githubusercontent.com/FrameworkComputer/zephyr/${ZEPHYR_REV}/scripts/requirements-base.txt /tmp/zephyr-requirements-base.txt
COPY requirements.txt /tmp/requirements.txt

RUN apt-get update -qq \
    && apt-get install -y -qq --no-install-recommends python3-pip \
    && rm -rf /var/lib/apt/lists/* \
    && pip3 install --no-cache-dir --break-system-packages \
        -r /tmp/zephyr-requirements-base.txt \
        -r /tmp/requirements.txt \
    && git config --system --add safe.directory '*'
