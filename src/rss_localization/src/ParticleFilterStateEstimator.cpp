#include "rss_grid_localization/ParticleFilterStateEstimator.h"
#include <cmath>
#include <random>
#include <algorithm>

namespace rss {

uniform_real_distribution<> ParticleFilterStateEstimator::uniformLinDist = uniform_real_distribution<>(0.0, 1.0);
normal_distribution<> ParticleFilterStateEstimator::normalDist = normal_distribution<>(0.0, 0.05);

random_device ParticleFilterStateEstimator::rd;
default_random_engine ParticleFilterStateEstimator::gen{ParticleFilterStateEstimator::rd()};

ParticleFilterStateEstimator::ParticleFilterStateEstimator(MeasurementModel *measurementModel,
                                                           MotionModel *motionModel, unsigned long particleCount)
    : particleCount(particleCount), measurementModel(measurementModel), motionModel(motionModel) {
  // Initialisation
}

void ParticleFilterStateEstimator::actionUpdate(Action action) {
  for (Particle &particle : particles) {
    particle.move(action);
  }

}

void ParticleFilterStateEstimator::measurementUpdate(const Measurement &z, const Map &m) {
  weights.clear();
  weights.reserve(particleCount);
  vector<double> tempWeights;
  tempWeights.reserve(particleCount);
  double total = 0.0;
  for (Particle &particle : particles) {
    double prob = particle.measurementProb(z, m);
    total += prob;
    tempWeights.push_back(prob);
  }
  // Normalise weights
  for (double &tempWeight : tempWeights) {
    tempWeight = tempWeight / total;
    weights.push_back(tempWeight);
  }
}

void ParticleFilterStateEstimator::particleUpdate() {
  ROS_DEBUG("Resampling");
  stochasticUniversalSampling();
}

void ParticleFilterStateEstimator::stochasticUniversalSampling() {
  double beta = uniformLinDist(gen) / double(particleCount);
  unsigned int index = 0;
  double increment = 1.0 / double(particleCount);
  vector<Particle> tempParticles;
  tempParticles.reserve(particleCount);
  for (Particle &particle : particles) {
    beta += increment;
    while (beta > weights[index]) {
      beta = beta - weights[index];
      index = (index + 1) % particleCount;
    };
    tempParticles.push_back(particles[index]);
  }
  for (unsigned long i = 0; i < particleCount; ++i) {
    particles[i] = tempParticles[i];
  }
}

void ParticleFilterStateEstimator::initialiseParticles(const Map &map, SimplePose pose) {
  ROS_DEBUG("Initialising Particles (%lu)", particleCount);
  // clear old particles
  particles.clear();
  weights.clear();

  particles.reserve(particleCount);
  weights.reserve(particleCount);

  SimplePose init = pose;
  for (unsigned long i = 0; i < particleCount; ++i) {
    SimplePose offset = {normalDist(gen), normalDist(gen), 0};
    SimplePose randPose = init + offset;
    particles.emplace_back(measurementModel, motionModel, randPose);
  }
}

void ParticleFilterStateEstimator::Particle::move(const Action &action) {
  pose = motionModel->run(pose, action);
}

double ParticleFilterStateEstimator::Particle::measurementProb(const Measurement &z, const Map &m) {
  return measurementModel->run(z, pose, m);
}

}