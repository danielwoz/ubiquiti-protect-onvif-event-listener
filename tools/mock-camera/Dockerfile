# MockCam: a simulated third-party ONVIF camera (see README.md).
#
# Base: mediamtx 1.21.2 with ffmpeg 8.1.2 on Alpine 3.24, pinned by digest, so the RTSP
# server and the encoder are exactly the versions this was tested with.
# Build with ./build.sh: it stages this directory plus the test images from test/testdata.
FROM bluenviron/mediamtx:1.21.2-ffmpeg@sha256:c0feb4acc548d5f410d57d9046f62dfa355e2abc4098f5243de9e86430c77a95

RUN apk add --no-cache python3~3.14 font-dejavu~2.37 tini~0.19

WORKDIR /opt/mockcam
COPY make-subjects.sh /opt/mockcam/
COPY subjects/ /tmp/subjects/
RUN sh /opt/mockcam/make-subjects.sh /tmp/subjects /opt/mockcam/assets/subjects \
    && rm -rf /tmp/subjects

COPY mockcam/ /opt/mockcam/mockcam/
RUN python3 -m compileall -q /opt/mockcam/mockcam \
    && adduser -D -H -u 10001 mockcam \
    && mkdir -p /run/mockcam && chown mockcam /run/mockcam

ARG MOCKCAM_FIRMWARE=dev
ENV MOCKCAM_RUN_DIR=/run/mockcam \
    MOCKCAM_ASSETS_DIR=/opt/mockcam/assets \
    MOCKCAM_FIRMWARE=${MOCKCAM_FIRMWARE} \
    PYTHONUNBUFFERED=1 \
    PYTHONDONTWRITEBYTECODE=1

USER 10001
# 8000 ONVIF SOAP + /snapshot.jpg, 8080 control API, 8554 RTSP (TCP only)
EXPOSE 8000 8080 8554
ENTRYPOINT ["/sbin/tini", "--", "python3", "-m", "mockcam"]
