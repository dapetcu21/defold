(ns internal.injection-test
  (:require [clojure.string :as str]
            [clojure.test :refer :all]
            [schema.core :as s]
            [schema.macros :as sm]
            [plumbing.core :refer [defnk]]
            [dynamo.node :as n]
            [dynamo.system :as ds :refer [add in]]
            [dynamo.system.test-support :refer :all]
            [internal.graph.lgraph :as lg]
            [internal.node :as in]))

(n/defnode4 Receiver
  (input surname String :inject)
  (input samples [s/Num] :inject)
  (input label s/Any :inject))

(defnk produce-string :- String
  []
  "nachname")

(n/defnode4 Sender1
  (output surname String produce-string))

(defnk produce-sample :- Integer
  []
  42)

(n/defnode4 Sampler
  (output sample Integer produce-sample))

(defnk produce-label :- s/Keyword
  []
  :a-keyword)

(n/defnode4 Labeler
  (output label s/Keyword produce-label))

(deftest compatible-inputs-and-outputs
  (let [recv    (n/construct Receiver)
        sender  (n/construct Sender1)
        sampler (n/construct Sampler)
        labeler (n/construct Labeler)]
    (is (= #{[sender  :surname recv :surname]} (in/injection-candidates [recv] [sender])))
    (is (= #{[sampler :sample  recv :samples]} (in/injection-candidates [recv] [sampler])))
    (is (= #{[labeler :label   recv :label]}   (in/injection-candidates [recv] [labeler])))))

(sm/defrecord CommonValueType
  [identifier :- String])

(defnk concat-all :- CommonValueType
  [local-names :- [CommonValueType]]
  (str/join (map :identifier local-names)))

(n/defnode4 ValueConsumer
  (input local-names [CommonValueType] :inject)
  (output concatenation CommonValueType concat-all))

(defnk passthrough [local-name] local-name)

(n/defnode4 InjectionScope
  (inherits n/Scope)
  (input local-name CommonValueType :inject)
  (output passthrough CommonValueType passthrough))

(n/defnode4 ValueProducer
  (property value CommonValueType)
  (output local-name CommonValueType (fn [this _] (:value this))))

(deftest dependency-injection
  (testing "attach node output to input on scope"
    (with-clean-world
      (let [scope (ds/transactional
                    (ds/in (ds/add (n/construct InjectionScope))
                      (ds/add (n/construct ValueProducer :value (CommonValueType. "a known value")))
                      (ds/current-scope)))]
        (is (= "a known value" (-> scope (n/get-node-value :passthrough) :identifier))))))

  (testing "attach one node output to input on another node"
    (with-clean-world
      (let [consumer (ds/transactional
                       (ds/in (ds/add (n/construct n/Scope))
                         (ds/add (n/construct ValueProducer :value (CommonValueType. "a known value")))
                         (ds/add (n/construct ValueConsumer))))]
        (is (= "a known value" (-> consumer (n/get-node-value :concatenation)))))))

  (testing "attach nodes in different transactions"
    (with-clean-world
      (let [scope (ds/transactional
                    (ds/add (n/construct n/Scope)))
            consumer (ds/transactional
                       (ds/in scope
                         (ds/add (n/construct ValueConsumer))))
            producer (ds/transactional
                       (ds/in scope
                         (ds/add (n/construct ValueProducer :value (CommonValueType. "a known value")))))]
        (is (= "a known value" (-> consumer (n/get-node-value :concatenation)))))))

  (testing "attach nodes in different transactions and reverse order"
    (with-clean-world
      (let [scope (ds/transactional
                    (ds/add (n/construct n/Scope)))
            producer (ds/transactional
                       (ds/in scope
                         (ds/add (n/construct ValueProducer :value (CommonValueType. "a known value")))))
            consumer (ds/transactional
                       (ds/in scope
                         (ds/add (n/construct ValueConsumer))))]
        (is (= "a known value" (-> consumer (n/get-node-value :concatenation)))))))

  (testing "explicitly connect nodes, see if injection also happens"
    (with-clean-world
      (let [scope (ds/transactional
                    (ds/add (n/construct n/Scope)))
            producer (ds/transactional
                       (ds/in scope
                         (ds/add (n/construct ValueProducer :value (CommonValueType. "a known value")))))
            consumer (ds/transactional
                       (ds/in scope
                         (let [c (ds/add (n/construct ValueConsumer))]
                           (ds/connect producer :local-name c :local-names)
                           c)))]
        (is (= "a known value" (-> consumer (n/get-node-value :concatenation))))))))

(n/defnode4 ReflexiveFeedback
  (property port s/Keyword (default :no))
  (input ports [s/Keyword] :inject))

(deftest reflexive-injection
  (testing "don't connect a node's own output to its input"
    (with-clean-world
      (let [node (ds/transactional (ds/add (n/construct ReflexiveFeedback)))]
        (is (not (lg/connected? (-> world-ref deref :graph) (:_id node) :port (:_id node) :ports)))))))
