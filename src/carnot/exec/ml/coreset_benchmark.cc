#include <benchmark/benchmark.h>

#include "src/carnot/exec/ml/coreset.h"
#include "src/common/perf/perf.h"

using pl::carnot::exec::ml::CoresetDriver;
using pl::carnot::exec::ml::CoresetTree;
using pl::carnot::exec::ml::KMeansCoreset;
using pl::carnot::exec::ml::WeightedPointSet;

// NOLINTNEXTLINE : runtime/references.
static void BM_CoresetTreeUpdate(benchmark::State& state) {
  // Create a coreset driver using the Coreset R-way tree data structure, and kmeans coresets.
  // Uses base buckets of size 64, points of size 64, 4-way tree, and coresets of size 64.
  int d = 64;
  CoresetDriver<CoresetTree<KMeansCoreset>> driver(64, d, 4, 64);
  Eigen::VectorXf point = Eigen::VectorXf::Random(d);

  for (auto _ : state) {
    driver.Update(point);
  }
}

// NOLINTNEXTLINE : runtime/references.
static void BM_CoresetTreeQuery(benchmark::State& state) {
  int d = 64;
  CoresetDriver<CoresetTree<KMeansCoreset>> driver(64, d, 4, 64);
  Eigen::VectorXf point = Eigen::VectorXf::Random(d);
  for (int i = 0; i < 10000; i++) {
    driver.Update(point);
  }

  for (auto _ : state) {
    benchmark::DoNotOptimize(driver.Query());
  }
}

// NOLINTNEXTLINE : runtime/references.
static void BM_CoresetTreeMerge(benchmark::State& state) {
  int d = 64;
  CoresetDriver<CoresetTree<KMeansCoreset>> driver1(64, d, 4, 64);
  CoresetDriver<CoresetTree<KMeansCoreset>> driver2(64, d, 4, 64);
  Eigen::VectorXf point = Eigen::VectorXf::Random(d);
  for (int i = 0; i < 10000; i++) {
    driver1.Update(point);
    driver2.Update(point);
  }

  for (auto _ : state) {
    driver1.Merge(driver2);
  }
}

// NOLINTNEXTLINE : runtime/references.
static void BM_CoresetFromWeightedPointSet(benchmark::State& state) {
  int d = 65;
  Eigen::MatrixXf points = Eigen::MatrixXf::Random(4 * 64, d);
  auto set = std::make_shared<WeightedPointSet>(points, Eigen::VectorXf::Ones(4 * 64));

  for (auto _ : state) {
    KMeansCoreset::FromWeightedPointSet(set, 64);
  }
}

// TODO(james): serialize/deserialize are a bit slow at the moment.
// NOLINTNEXTLINE : runtime/references.
static void BM_CoresetSerialize(benchmark::State& state) {
  int d = 64;
  CoresetDriver<CoresetTree<KMeansCoreset>> driver(64, d, 4, 64);
  Eigen::VectorXf point = Eigen::VectorXf::Random(d);
  for (int i = 0; i < 10000; i++) {
    driver.Update(point);
  }

  for (auto _ : state) {
    benchmark::DoNotOptimize(driver.ToJSON());
  }
}

// NOLINTNEXTLINE : runtime/references.
static void BM_CoresetDeserialize(benchmark::State& state) {
  int d = 64;
  CoresetDriver<CoresetTree<KMeansCoreset>> driver(64, d, 4, 64);
  Eigen::VectorXf point = Eigen::VectorXf::Random(d);
  for (int i = 0; i < 10000; i++) {
    driver.Update(point);
  }
  auto serialized = driver.ToJSON();

  CoresetDriver<CoresetTree<KMeansCoreset>> driver2(64, d, 4, 64);

  for (auto _ : state) {
    driver2.FromJSON(serialized);
  }
}

BENCHMARK(BM_CoresetTreeUpdate);
BENCHMARK(BM_CoresetFromWeightedPointSet);
BENCHMARK(BM_CoresetTreeQuery);
BENCHMARK(BM_CoresetTreeMerge);
BENCHMARK(BM_CoresetSerialize);
BENCHMARK(BM_CoresetDeserialize);