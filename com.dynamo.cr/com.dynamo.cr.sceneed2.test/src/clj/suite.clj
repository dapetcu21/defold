(ns suite
 (:require [clojure.test :as test]))

(def test-namespaces ['internal.injection-test
                      'internal.node-test
                      'internal.scope-test
                      'internal.system-test
                      'internal.type-test
                      'internal.either-test
                      'internal.value-test
                      'internal.graph.graph-test
                      'internal.render.pass-test
                      'dynamo.camera-test
                      'dynamo.condition-test
                      'dynamo.geom-test
                      'dynamo.image-test
                      'dynamo.protobuf-test
                      'dynamo.texture-test
                      'dynamo.transaction-test
                      'dynamo.gl.translate-test
                      'dynamo.gl.vertex-test
                      'docs])

(def test-namespaces-for-junit
  (into-array String (map name test-namespaces)))

(defn suite []
  (doseq [test-ns test-namespaces]
    (require test-ns)
    (test/run-tests test-ns)))
