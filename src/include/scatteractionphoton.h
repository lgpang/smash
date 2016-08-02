/*
 *
 *    Copyright (c) 2016
 *      SMASH Team
 *
 *    GNU General Public License (GPLv3 or later)
 *
 */

#ifndef SRC_INCLUDE_SCATTERACTIONPHOTON_H_
#define SRC_INCLUDE_SCATTERACTIONPHOTON_H_

#include "constants.h"
#include "scatteraction.h"

namespace Smash {

class ScatterActionPhoton : public ScatterAction {
 public:
  ScatterActionPhoton(const ParticleData &in_part1,
                      const ParticleData &in_part2, float time, int nofp)
      : ScatterAction(in_part1, in_part2, time),
        number_of_fractional_photons(nofp) {}

  void generate_final_state() override;

  void add_all_processes(float elastic_parameter,
                         bool two_to_one, bool two_to_two,
                         bool strings_switch) override;

  float raw_weight_value() const override { return weight_; }

  float cross_section() const override {
    if (cross_section_photons_ < really_small) {
      return cross_section_photons_;
    } else {
      return total_cross_section_ + cross_section_photons_;
    }
  }

  /** Overridden to effectively return the reaction channel. */
  virtual ProcessType get_type() const override {
    return static_cast<ProcessType>(reac);
  }

  /** To add only one reaction for testing purposes */
  void add_single_channel(){
    add_processes<CollisionBranch>(photon_cross_sections(),
                                  collision_channels_photons_,
                                  cross_section_photons_);
  }

 private:

  CollisionBranchList photon_cross_sections();

  int const number_of_fractional_photons;

  float weight_ = 0.0;

  /** List of possible collisions producing photons */
  CollisionBranchList collision_channels_photons_;

  float cross_section_photons_ = 0.0;

  const int num_tab_pts = 200;

  enum class ReactionType {
    no_reaction = 0,
    pi0_pi = 1,
    piplus_rho0 = 2,
    pi_rho = 3,
    pi0_rho = 4,
    piplus_eta = 5,
    pi_pi = 6
  };

  ReactionType reac = ReactionType::no_reaction;

  float pi_pi_rho0(const float M, const float s) const;

  float pi_pi0_rho(const float M, const float s) const;

  float diff_cross_section(float t, float m3) const;
};

}  // namespace Smash

#endif  // SRC_INCLUDE_SCATTERACTIONPHOTON_H_
