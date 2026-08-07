# Differential corpus sources

The corpus was supplied for parser testing and contains these real schemas:

- [`descriptor.proto`](corpus/descriptor.proto) — Protocol Buffers [`descriptor.proto`](https://github.com/protocolbuffers/protobuf/blob/main/src/google/protobuf/descriptor.proto);
- [`onnx.proto3`](corpus/onnx.proto3) — [ONNX](https://github.com/onnx/onnx/blob/main/onnx/onnx.proto3);
- [`caffe.proto`](corpus/caffe.proto) — [BVLC Caffe](https://github.com/BVLC/caffe/blob/master/src/caffe/proto/caffe.proto);
- [`PulsarApi.proto`](corpus/PulsarApi.proto) — [Apache Pulsar](https://github.com/apache/pulsar/blob/master/pulsar-common/src/main/proto/PulsarApi.proto);
- [`mesos.proto`](corpus/mesos.proto) — [Apache Mesos](https://github.com/apache/mesos/blob/master/include/mesos/mesos.proto);
- [`envoy-route-components.proto`](corpus/envoy-route-components.proto) — [Envoy route components](https://github.com/envoyproxy/envoy/blob/main/api/envoy/config/route/v3/route_components.proto);
- [`kubernetes-core-v1-generated.proto`](corpus/kubernetes-core-v1-generated.proto) — [Kubernetes core API](https://github.com/kubernetes/api/blob/master/core/v1/generated.proto).

The [`stubs/`](stubs/) tree contains minimal imported declarations needed only so an external [`protoc`](https://github.com/protocolbuffers/protobuf) can resolve and validate the two schemas whose complete upstream dependency trees are not included. These stubs are not production parser inputs and are not claims of complete upstream APIs.
