"""Reusable gRPC ext_proc client for Ranvier's GIE Endpoint-Picker (EPP).

Drives the `envoy.service.ext_proc.v3.ExternalProcessor/Process` bidi stream the
way a GIE-conformant gateway would, and parses the picker's response: the
chosen `<ip:port>` (from the `x-gateway-destination-endpoint` header mutation
and/or `dynamic_metadata`), or the `ImmediateResponse` HTTP status (e.g. 503
when no backend is ready).

Used by BOTH:
  * tests/integration/test_gie_epp.py  — behavioural assertions, and
  * the EPP overhead microbenchmark      — timing pick_endpoint() in a loop.

The protobuf/gRPC Python stubs are generated at runtime from the same
proto/ext_proc_min.proto the C++ server is built from (no committed,
version-sensitive *_pb2.py). Requires `grpcio` + `grpcio-tools`
(tests/integration/requirements.txt); callers should importorskip this module.

Build constraint: needs a running EPP-enabled Ranvier (WITH_GIE_EPP=ON,
gie_epp.enabled). Do not attempt to run in the sandbox — the developer runs
this in their Docker environment.
"""

from __future__ import annotations

import os
import sys
import tempfile
from dataclasses import dataclass, field
from typing import Dict, Iterator, Optional, Tuple

import grpc  # type: ignore

# The header the data plane reads to learn the chosen backend(s).
DEST_ENDPOINT_HEADER = "x-gateway-destination-endpoint"
# Dynamic-metadata namespace the EPP writes the endpoint under.
DYNAMIC_METADATA_NAMESPACE = "envoy.lb"

# ext_proc service path components (must match proto/ext_proc_min.proto).
SERVICE_NAME = "envoy.service.ext_proc.v3.ExternalProcessor"

_REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
_PROTO_DIR = os.path.join(_REPO_ROOT, "proto")
_PROTO_FILE = "ext_proc_min.proto"

# Generated modules, populated once by _ensure_stubs().
_pb2 = None
_pb2_grpc = None


def _ensure_stubs():
    """Generate + import the ext_proc Python stubs once, from the vendored proto.

    Generating at import time (rather than committing *_pb2.py) keeps the stubs
    matched to the installed protobuf runtime and the proto in lockstep with the
    C++ build. Idempotent: caches the imported modules in module globals.
    """
    global _pb2, _pb2_grpc
    if _pb2 is not None and _pb2_grpc is not None:
        return _pb2, _pb2_grpc

    from grpc_tools import protoc  # type: ignore

    out_dir = os.path.join(tempfile.gettempdir(), "ranvier_epp_stubs")
    os.makedirs(out_dir, exist_ok=True)

    rc = protoc.main(
        [
            "grpc_tools.protoc",
            f"-I{_PROTO_DIR}",
            f"--python_out={out_dir}",
            f"--grpc_python_out={out_dir}",
            _PROTO_FILE,
        ]
    )
    if rc != 0:
        raise RuntimeError(
            f"protoc failed (rc={rc}) generating ext_proc stubs from "
            f"{os.path.join(_PROTO_DIR, _PROTO_FILE)}"
        )

    if out_dir not in sys.path:
        sys.path.insert(0, out_dir)
    import ext_proc_min_pb2 as pb2  # type: ignore
    import ext_proc_min_pb2_grpc as pb2_grpc  # type: ignore

    _pb2, _pb2_grpc = pb2, pb2_grpc
    return _pb2, _pb2_grpc


@dataclass
class PickResult:
    """Outcome of one Process() call against the EPP."""

    endpoint: Optional[str] = None          # "<ip:port>" from the header mutation
    metadata_endpoint: Optional[str] = None  # same value via dynamic_metadata
    immediate_status: Optional[int] = None   # HTTP status of an ImmediateResponse (e.g. 503)
    raw_responses: int = 0                   # number of ProcessingResponses seen

    @property
    def has_endpoint(self) -> bool:
        return bool(self.endpoint)


def _build_request_stream(pb2, prompt: str, model: str, send_body: bool) -> Iterator:
    """Yield the ProcessingRequest sequence a gateway sends for one request.

    request_headers (end_of_stream = not send_body) then, when send_body, a
    single BUFFERED request_body frame carrying the chat-completions JSON body
    the EPP tokenizes for prefix-aware routing.
    """
    headers = pb2.ProcessingRequest()
    headers.request_headers.end_of_stream = not send_body
    # A couple of representative request headers (content unused by the picker
    # in this build, but present so the message mirrors a real gateway call).
    hv = headers.request_headers.headers.headers.add()
    hv.key = ":path"
    hv.value = "/v1/chat/completions"
    yield headers

    if send_body:
        import json

        body_json = json.dumps(
            {"model": model, "messages": [{"role": "user", "content": prompt}]}
        ).encode("utf-8")
        body = pb2.ProcessingRequest()
        body.request_body.body = body_json
        body.request_body.end_of_stream = True
        yield body


def _parse_response(pb2, resp, result: PickResult) -> None:
    """Fold one ProcessingResponse into `result` (endpoint / metadata / 503)."""
    result.raw_responses += 1

    if resp.HasField("immediate_response"):
        result.immediate_status = resp.immediate_response.status.code
        return

    # Header mutation may ride on either the request_headers or request_body
    # phase response (the EPP decides on whichever phase completes the body).
    common = None
    if resp.HasField("request_headers"):
        common = resp.request_headers.response
    elif resp.HasField("request_body"):
        common = resp.request_body.response
    if common is not None:
        for opt in common.header_mutation.set_headers:
            if opt.header.key == DEST_ENDPOINT_HEADER and opt.header.value:
                result.endpoint = opt.header.value

    # dynamic_metadata: { "envoy.lb": { "<hdr>": "<ip:port>" } }
    # (use `in`/[] — protobuf map containers don't reliably expose .get())
    if resp.HasField("dynamic_metadata"):
        fields = resp.dynamic_metadata.fields
        if DYNAMIC_METADATA_NAMESPACE in fields:
            ns = fields[DYNAMIC_METADATA_NAMESPACE]
            if ns.HasField("struct_value"):
                inner = ns.struct_value.fields
                if DEST_ENDPOINT_HEADER in inner and inner[DEST_ENDPOINT_HEADER].string_value:
                    result.metadata_endpoint = inner[DEST_ENDPOINT_HEADER].string_value


def pick_endpoint_on_channel(
    channel,
    prompt: str = "hello world from the ranvier epp integration test",
    *,
    model: str = "test-model",
    send_body: bool = True,
    timeout: float = 10.0,
) -> PickResult:
    """Run one Process() decision on an EXISTING channel.

    The microbenchmark reuses a single channel across many calls so the timed
    quantity is the EPP's routing-decision + bridge overhead, not per-call
    channel setup.
    """
    pb2, pb2_grpc = _ensure_stubs()
    result = PickResult()
    stub = pb2_grpc.ExternalProcessorStub(channel)
    responses = stub.Process(
        _build_request_stream(pb2, prompt, model, send_body), timeout=timeout
    )
    for resp in responses:
        _parse_response(pb2, resp, result)
    return result


def pick_endpoint(
    target: str,
    prompt: str = "hello world from the ranvier epp integration test",
    *,
    model: str = "test-model",
    send_body: bool = True,
    timeout: float = 10.0,
) -> PickResult:
    """Open a Process() stream to the EPP at `target` ("host:port") and decide.

    Returns a :class:`PickResult`. A fresh insecure channel is used per call so
    the function is self-contained for tests; use
    :func:`pick_endpoint_on_channel` for tight timing loops.
    """
    with grpc.insecure_channel(target) as channel:
        return pick_endpoint_on_channel(
            channel, prompt, model=model, send_body=send_body, timeout=timeout
        )
