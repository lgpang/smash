/*
 *
 *    Copyright (c) 2014-2017
 *      SMASH Team
 *
 *    GNU General Public License (GPLv3 or later)
 *
 */

#include "include/scatteraction.h"

#include <cmath>

#include "Pythia8/Pythia.h"

#include "include/action_globals.h"
#include "include/constants.h"
#include "include/cxx14compat.h"
#include "include/fpenvironment.h"
#include "include/kinematics.h"
#include "include/logging.h"
#include "include/parametrizations.h"
#include "include/pdgcode.h"
#include "include/processstring.h"
#include "include/random.h"

namespace smash {

ScatterAction::ScatterAction(const ParticleData &in_part_a,
                             const ParticleData &in_part_b,
                             double time, bool isotropic,
                             double string_formation_time)
    : Action({in_part_a, in_part_b}, time),
      total_cross_section_(0.), isotropic_(isotropic),
      string_formation_time_(string_formation_time) {}

void ScatterAction::add_collision(CollisionBranchPtr p) {
  add_process<CollisionBranch>(p, collision_channels_, total_cross_section_);
}

void ScatterAction::add_collisions(CollisionBranchList pv) {
  add_processes<CollisionBranch>(std::move(pv), collision_channels_,
                                 total_cross_section_);
}

void ScatterAction::generate_final_state() {
  const auto &log = logger<LogArea::ScatterAction>();

  log.debug("Incoming particles: ", incoming_particles_);

  if (pot_pointer != nullptr) {
    filter_channel(collision_channels_, total_cross_section_);
  }
  /* Decide for a particular final state. */
  const CollisionBranch *proc = choose_channel<CollisionBranch>(
      collision_channels_, total_cross_section_);
  process_type_ = proc->get_type();
  outgoing_particles_ = proc->particle_list();
  partial_cross_section_ = proc->weight();

  log.debug("Chosen channel: ", process_type_, outgoing_particles_);

  /* The production point of the new particles.  */
  FourVector middle_point = get_interaction_point();

  switch (process_type_) {
    case ProcessType::Elastic:
      /* 2->2 elastic scattering */
      elastic_scattering();
      break;
    case ProcessType::TwoToOne:
      /* resonance formation */
      /* processes computed in the center of momenta */
      resonance_formation();
      break;
    case ProcessType::TwoToTwo:
      /* 2->2 inelastic scattering */
      /* Sample the particle momenta in CM system. */
      inelastic_scattering();
      break;
    case ProcessType::StringSoft:
      /* soft string excitation */
      string_excitation_soft();
      break;
    case ProcessType::StringHard:
      /* hard string excitation */
      string_excitation_pythia();
      break;
    default:
      throw InvalidScatterAction(
          "ScatterAction::generate_final_state: Invalid process type " +
          std::to_string(static_cast<int>(process_type_)) + " was requested. " +
          "(PDGcode1=" + incoming_particles_[0].pdgcode().string() +
          ", PDGcode2=" + incoming_particles_[1].pdgcode().string() + ")");
  }

  for (ParticleData &new_particle : outgoing_particles_) {
    /* Set positions of the outgoing particles */
    if (proc->get_type() != ProcessType::Elastic) {
      new_particle.set_4position(middle_point);
    }
    /* Set momenta & boost to computational frame. */
    new_particle.boost_momentum(-beta_cm());
  }
}


void ScatterAction::add_all_processes(double elastic_parameter,
                                      bool two_to_one,
                                      ReactionsBitSet included_2to2,
                                      double low_snn_cut,
                                      bool strings_switch,
                                      NNbarTreatment nnbar_treatment) {
  /* The string fragmentation is implemented in the same way in GiBUU (Physics
   * Reports 512(2012), 1-124, pg. 33). If the center of mass energy is low, two
   * particles scatter through the resonance channels. If high, the out going
   * particles are generated by string fragmentation. If in between, the out
   * going
   * particles are generated either through the resonance channels or string
   * fragmentation by chance. In detail, the low energy regoin is from the
   * threshold to (mix_scatter_type_energy - mix_scatter_type_window_width),
   * while
   * the high energy region is from (mix_scatter_type_energy +
   * mix_scatter_type_window_width) to infinity. In between, the probability for
   * string fragmentation increases linearly from 0 to 1 as the c.m. energy.*/
  // Determine the energy region of the mixed scattering type for two types of
  // scattering.
  const ParticleType &t1 = incoming_particles_[0].type();
  const ParticleType &t2 = incoming_particles_[1].type();
  const bool both_are_nucleons = t1.is_nucleon() && t2.is_nucleon();
  bool include_pythia = false;
  double mix_scatter_type_energy;
  double mix_scatter_type_window_width;
  if (both_are_nucleons) {
    // The energy region of the mixed scattering type for nucleon-nucleon
    // collision is 4.0 - 5.0 GeV.
    mix_scatter_type_energy = 4.5;
    mix_scatter_type_window_width = 0.5;
    // nucleon-nucleon collisions are included in pythia.
    include_pythia = true;
  } else if ((t1.pdgcode().is_pion() && t2.is_nucleon()) ||
             (t1.is_nucleon() && t2.pdgcode().is_pion())) {
    // The energy region of the mixed scattering type for pion-nucleon collision
    // is 1.9 - 2.2 GeV.
    mix_scatter_type_energy = 2.05;
    mix_scatter_type_window_width = 0.15;
    // pion-nucleon collisions are included in pythia.
    include_pythia = true;
  }
  // string fragmentation is enabled when strings_switch is on and the process
  // is included in pythia.
  const bool enable_pythia = strings_switch && include_pythia;
  // Whether the scattering is through string fragmentaion
  bool is_pythia = false;
  if (enable_pythia) {
    if (sqrt_s() > mix_scatter_type_energy + mix_scatter_type_window_width) {
      // scatterings at high energies are through string fragmentation
      is_pythia = true;
    } else if (sqrt_s() >
               mix_scatter_type_energy - mix_scatter_type_window_width) {
          const double probability_pythia = 0.5 +
                     0.5 * sin(0.5 * M_PI * (sqrt_s() - mix_scatter_type_energy)
                     / mix_scatter_type_window_width);
      if (probability_pythia > Random::uniform(0., 1.)) {
        // scatterings at the middle energies are through string
        // fragmentation by chance.
        is_pythia = true;
      }
    }
  }
    /** Elastic collisions between two nucleons with sqrt_s() below
     * low_snn_cut can not happen*/
  const bool reject_by_nucleon_elastic_cutoff = both_are_nucleons
                         && t1.antiparticle_sign() == t2.antiparticle_sign()
                         && sqrt_s() < low_snn_cut;
  bool incl_elastic = included_2to2[IncludedReactions::Elastic];
  if (incl_elastic && !reject_by_nucleon_elastic_cutoff) {
      add_collision(elastic_cross_section(elastic_parameter));
  }
  if (is_pythia) {
    /* string excitation */
    add_collisions(string_excitation_cross_sections());
  } else {
     if (two_to_one) {
       /* resonance formation (2->1) */
       add_collisions(resonance_cross_sections());
     }
     if (included_2to2.any()) {
       /* 2->2 (inelastic) */
       add_collisions(two_to_two_cross_sections(included_2to2));
     }
  }
  /** NNbar annihilation thru NNbar → ρh₁(1170); combined with the decays
   *  ρ → ππ and h₁(1170) → πρ, this gives a final state of 5 pions.
   *  Only use in cases when detailed balance MUST happen, i.e. in a box! */
  if (nnbar_treatment == NNbarTreatment::Resonances) {
    if (t1.is_nucleon() && t2.pdgcode() == t1.get_antiparticle()->pdgcode()) {
      add_collision(NNbar_annihilation_cross_section());
    }
    if ((t1.pdgcode() == pdg::rho_z && t2.pdgcode() == pdg::h1) ||
        (t1.pdgcode() == pdg::h1 && t2.pdgcode() == pdg::rho_z)) {
      add_collisions(NNbar_creation_cross_section());
    }
  }
}

double ScatterAction::raw_weight_value() const { return total_cross_section_; }

double ScatterAction::partial_weight() const { return partial_cross_section_; }

ThreeVector ScatterAction::beta_cm() const {
  return total_momentum().velocity();
}

double ScatterAction::gamma_cm() const {
  return (1. / std::sqrt(1.0 - beta_cm().sqr()));
}

double ScatterAction::mandelstam_s() const { return total_momentum().sqr(); }

double ScatterAction::cm_momentum() const {
  const double m1 = incoming_particles_[0].effective_mass();
  const double m2 = incoming_particles_[1].effective_mass();
  return pCM(sqrt_s(), m1, m2);
}

double ScatterAction::cm_momentum_squared() const {
  const double m1 = incoming_particles_[0].effective_mass();
  const double m2 = incoming_particles_[1].effective_mass();
  return pCM_sqr(sqrt_s(), m1, m2);
}

double ScatterAction::transverse_distance_sqr() const {
  // local copy of particles (since we need to boost them)
  ParticleData p_a = incoming_particles_[0];
  ParticleData p_b = incoming_particles_[1];
  /* Boost particles to center-of-momentum frame. */
  const ThreeVector velocity = beta_cm();
  p_a.boost(velocity);
  p_b.boost(velocity);
  const ThreeVector pos_diff =
      p_a.position().threevec() - p_b.position().threevec();
  const ThreeVector mom_diff =
      p_a.momentum().threevec() - p_b.momentum().threevec();

  const auto &log = logger<LogArea::ScatterAction>();
  log.debug("Particle ", incoming_particles_,
            " position difference [fm]: ", pos_diff,
            ", momentum difference [GeV]: ", mom_diff);

  const double dp2 = mom_diff.sqr();
  const double dr2 = pos_diff.sqr();
  /* Zero momentum leads to infite distance. */
  if (dp2 < really_small) {
    return dr2;
  }
  const double dpdr = pos_diff * mom_diff;

  /** UrQMD squared distance criterion:
   * \iref{Bass:1998ca} (3.27): in center of momentum frame
   * position of particle a: x_a
   * position of particle b: x_b
   * momentum of particle a: p_a
   * momentum of particle b: p_b
   * d^2_{coll} = (x_a - x_b)^2 - ((x_a - x_b) . (p_a - p_b))^2 / (p_a - p_b)^2
   */
  return dr2 - dpdr * dpdr / dp2;
}

CollisionBranchPtr ScatterAction::elastic_cross_section(double elast_par) {
  double elastic_xs;
  if (elast_par >= 0.) {
    // use constant elastic cross section from config file
    elastic_xs = elast_par;
  } else {
    // use parametrization
    elastic_xs = elastic_parametrization();
  }
  return make_unique<CollisionBranch>(incoming_particles_[0].type(),
                                      incoming_particles_[1].type(), elastic_xs,
                                      ProcessType::Elastic);
}

CollisionBranchPtr ScatterAction::NNbar_annihilation_cross_section() {
  const auto &log = logger<LogArea::ScatterAction>();
  /* Calculate NNbar cross section:
   * Parametrized total minus all other present channels.*/
  double nnbar_xsec = std::max(0., total_cross_section() - cross_section());
  log.debug("NNbar cross section is: ", nnbar_xsec);
  // Make collision channel NNbar -> ρh₁(1170); eventually decays into 5π
  return make_unique<CollisionBranch>(ParticleType::find(pdg::h1),
                                      ParticleType::find(pdg::rho_z),
                                      nnbar_xsec, ProcessType::TwoToTwo);
}

CollisionBranchList ScatterAction::NNbar_creation_cross_section() {
  const auto &log = logger<LogArea::ScatterAction>();
  CollisionBranchList channel_list;
  /* Calculate NNbar reverse cross section:
   * from reverse reaction (see NNbar_annihilation_cross_section).*/
  const double s = mandelstam_s();
  const double sqrts = sqrt_s();
  const double pcm = cm_momentum();

  const auto &type_N = ParticleType::find(pdg::p);
  const auto &type_Nbar = ParticleType::find(-pdg::p);

  // Check available energy
  if (sqrts - 2 * type_N.mass() < 0) {
    return channel_list;
  }

  double xsection = detailed_balance_factor_RR(
                        sqrts, pcm, incoming_particles_[0].type(),
                        incoming_particles_[1].type(), type_N, type_Nbar) *
                    std::max(0., ppbar_total(s) - ppbar_elastic(s));
  log.debug("NNbar reverse cross section is: ", xsection);
  channel_list.push_back(make_unique<CollisionBranch>(
      type_N, type_Nbar, xsection, ProcessType::TwoToTwo));
  channel_list.push_back(make_unique<CollisionBranch>(
      ParticleType::find(pdg::n), ParticleType::find(-pdg::n), xsection,
      ProcessType::TwoToTwo));
  return channel_list;
}

CollisionBranchPtr ScatterAction::string_excitation_cross_section() {
  const auto &log = logger<LogArea::ScatterAction>();
  /* Calculate string-excitation cross section:
   * Parametrized total minus all other present channels. */
  double sig_string =
      std::max(0., high_energy_cross_section() - elastic_parametrization());
  log.debug("String cross section is: ", sig_string);
  return make_unique<CollisionBranch>(sig_string, ProcessType::StringHard);
}

CollisionBranchList ScatterAction::string_excitation_cross_sections() {
  const auto &log = logger<LogArea::ScatterAction>();
  /* Calculate string-excitation cross section:
   * Parametrized total minus all other present channels. */
  double sig_string_all =
      std::max(0., high_energy_cross_section() - elastic_parametrization());

  /* get PDG id for evaluation of the parametrized cross sections
   * for diffractive processes.
   * (anti-)proton is used for (anti-)baryons and
   * pion is used for mesons.
   * This must be rescaled according to additive quark model
   * in the case of exotic hadrons. */
  std::array<int, 2> pdgid;
  for (int i = 0; i < 2; i++) {
    PdgCode pdg = incoming_particles_[i].type().pdgcode();
    pdg.deexcite();
    if (pdg.baryon_number() == 1) {
      pdgid[i] = 2212;
    } else if (pdg.baryon_number() == -1) {
      pdgid[i] = -2212;
    } else {
      pdgid[i] = 211;
    }
  }

  CollisionBranchList channel_list;
  if (sig_string_all > 0.) {
    /* Total parametrized cross-section (I) and pythia-produced total
     * cross-section (II) do not necessarily coincide. If I > II then
     * non-diffractive cross-section is reinforced to get I == II.
     * If I < II then partial cross-sections are drained one-by-one
     * to reduce II until I == II:
     * first non-diffractive, then double-diffractive, then
     * single-diffractive AB->AX and AB->XB in equal proportion.
     * The way it is done here is not unique. I (ryu) think that at high energy
     * collision this is not an issue, but at sqrt_s < 10 GeV it may
     * matter. */
    if (!string_process_) {
      throw std::runtime_error("string_process_ should be initialized.");
    }
    std::array<double, 3> xs = string_process_->cross_sections_diffractive(
        pdgid[0], pdgid[1], sqrt_s());
    double single_diffr_AX = xs[0], single_diffr_XB = xs[1],
           double_diffr = xs[2];
    double single_diffr = single_diffr_AX + single_diffr_XB;
    double diffractive = single_diffr + double_diffr;
    const double nondiffractive_all =
        std::max(0., sig_string_all - diffractive);
    diffractive = sig_string_all - nondiffractive_all;
    double_diffr = std::max(0., diffractive - single_diffr);
    const double a = (diffractive - double_diffr) / single_diffr;
    single_diffr_AX *= a;
    single_diffr_XB *= a;
    assert(std::abs(single_diffr_AX + single_diffr_XB + double_diffr +
                    nondiffractive_all - sig_string_all) < 1.e-6);

    /* Hard string process is added by hard cross section
     * in conjunction with multipartion interaction picture
     * \iref{Sjostrand:1987su}. */
    const double hard_xsec = string_hard_cross_section();
    const double nondiffractive_soft =
        nondiffractive_all * std::exp(-hard_xsec / nondiffractive_all);
    const double nondiffractive_hard = nondiffractive_all - nondiffractive_soft;
    log.debug("String cross sections [mb] are");
    log.debug("Single-diffractive AB->AX: ", single_diffr_AX);
    log.debug("Single-diffractive AB->XB: ", single_diffr_XB);
    log.debug("Double-diffractive AB->XX: ", double_diffr);
    log.debug("Soft non-diffractive: ", nondiffractive_soft);
    log.debug("Hard non-diffractive: ", nondiffractive_hard);
    /* cross section of soft string excitation */
    const double sig_string_soft = sig_string_all - nondiffractive_hard;

    /* fill cross section arrays */
    std::array<double, 5> string_sub_cross_sections;
    string_sub_cross_sections[0] = single_diffr_AX;
    string_sub_cross_sections[1] = single_diffr_XB;
    string_sub_cross_sections[2] = double_diffr;
    string_sub_cross_sections[3] = nondiffractive_soft;
    string_sub_cross_sections[4] = nondiffractive_hard;
    string_sub_cross_sections_sum_[0] = 0.;
    for (int i = 0; i < 5; i++) {
      string_sub_cross_sections_sum_[i + 1] =
          string_sub_cross_sections_sum_[i] + string_sub_cross_sections[i];
    }

    /* fill the list of process channels */
    if (sig_string_soft > 0.) {
      channel_list.push_back(make_unique<CollisionBranch>(
          sig_string_soft, ProcessType::StringSoft));
    }
    if (nondiffractive_hard > 0.) {
      channel_list.push_back(make_unique<CollisionBranch>(
          nondiffractive_hard, ProcessType::StringHard));
    }
  }
  return channel_list;
}

double ScatterAction::two_to_one_formation(const ParticleType &type_resonance,
                                           double srts,
                                           double cm_momentum_sqr) {
  const ParticleType &type_particle_a = incoming_particles_[0].type();
  const ParticleType &type_particle_b = incoming_particles_[1].type();
  /* Check for charge conservation. */
  if (type_resonance.charge() !=
      type_particle_a.charge() + type_particle_b.charge()) {
    return 0.;
  }

  /* Check for baryon-number conservation. */
  if (type_resonance.baryon_number() !=
      type_particle_a.baryon_number() + type_particle_b.baryon_number()) {
    return 0.;
  }

  /* Calculate partial in-width. */
  const double partial_width = type_resonance.get_partial_in_width(
      srts, incoming_particles_[0], incoming_particles_[1]);
  if (partial_width <= 0.) {
    return 0.;
  }

  /* Calculate spin factor */
  const double spinfactor =
      static_cast<double>(type_resonance.spin() + 1) /
      ((type_particle_a.spin() + 1) * (type_particle_b.spin() + 1));
  const int sym_factor =
      (type_particle_a.pdgcode() == type_particle_b.pdgcode()) ? 2 : 1;
  /** Calculate resonance production cross section
   * using the Breit-Wigner distribution as probability amplitude.
   * See Eq. (176) in \iref{Buss:2011mx}. */
  return spinfactor * sym_factor * 2. * M_PI * M_PI / cm_momentum_sqr *
         type_resonance.spectral_function(srts) * partial_width * hbarc *
         hbarc / fm2_mb;
}

CollisionBranchList ScatterAction::resonance_cross_sections() {
  const auto &log = logger<LogArea::ScatterAction>();
  CollisionBranchList resonance_process_list;
  const ParticleType &type_particle_a = incoming_particles_[0].type();
  const ParticleType &type_particle_b = incoming_particles_[1].type();

  const double srts = sqrt_s();
  const double p_cm_sqr = cm_momentum_squared();

  /* Find all the possible resonances */
  for (const ParticleType &type_resonance : ParticleType::list_all()) {
    /* Not a resonance, go to next type of particle */
    if (type_resonance.is_stable()) {
      continue;
    }

    /* Same resonance as in the beginning, ignore */
    if ((!type_particle_a.is_stable() &&
         type_resonance.pdgcode() == type_particle_a.pdgcode()) ||
        (!type_particle_b.is_stable() &&
         type_resonance.pdgcode() == type_particle_b.pdgcode())) {
      continue;
    }

    double resonance_xsection =
        two_to_one_formation(type_resonance, srts, p_cm_sqr);

    /* If cross section is non-negligible, add resonance to the list */
    if (resonance_xsection > really_small) {
      resonance_process_list.push_back(make_unique<CollisionBranch>(
          type_resonance, resonance_xsection, ProcessType::TwoToOne));
      log.debug("Found resonance: ", type_resonance);
      log.debug(type_particle_a.name(), type_particle_b.name(), "->",
               type_resonance.name(), " at sqrt(s)[GeV] = ", srts,
               " with xs[mb] = ", resonance_xsection);
    }
  }
  return resonance_process_list;
}

void ScatterAction::elastic_scattering() {
  // copy initial particles into final state
  outgoing_particles_[0] = incoming_particles_[0];
  outgoing_particles_[1] = incoming_particles_[1];
  // resample momenta
  sample_angles({outgoing_particles_[0].effective_mass(),
                 outgoing_particles_[1].effective_mass()});
}

void ScatterAction::inelastic_scattering() {
  // create new particles
  sample_2body_phasespace();
  /* Set the formation time of the 2 particles to the larger formation time of
   * the
   * incoming particles, if it is larger than the execution time; execution time
   * is otherwise taken to be the formation time */
  const double t0 = incoming_particles_[0].formation_time();
  const double t1 = incoming_particles_[1].formation_time();

  const size_t index_tmax = (t0 > t1) ? 0 : 1;
  const double sc =
      incoming_particles_[index_tmax].cross_section_scaling_factor();
  if (t0 > time_of_execution_ || t1 > time_of_execution_) {
    outgoing_particles_[0].set_formation_time(std::max(t0, t1));
    outgoing_particles_[1].set_formation_time(std::max(t0, t1));
    outgoing_particles_[0].set_cross_section_scaling_factor(sc);
    outgoing_particles_[1].set_cross_section_scaling_factor(sc);
  } else {
    outgoing_particles_[0].set_formation_time(time_of_execution_);
    outgoing_particles_[1].set_formation_time(time_of_execution_);
  }
}

void ScatterAction::resonance_formation() {
  const auto &log = logger<LogArea::ScatterAction>();

  if (outgoing_particles_.size() != 1) {
    std::string s =
        "resonance_formation: "
        "Incorrect number of particles in final state: ";
    s += std::to_string(outgoing_particles_.size()) + " (";
    s += incoming_particles_[0].pdgcode().string() + " + ";
    s += incoming_particles_[1].pdgcode().string() + ")";
    throw InvalidResonanceFormation(s);
  }

  const double cms_kin_energy = kinetic_energy_cms();
  /* 1 particle in final state: Center-of-momentum frame of initial particles
   * is the rest frame of the resonance.  */
  outgoing_particles_[0].set_4momentum(FourVector(cms_kin_energy, 0., 0., 0.));

  /* Set the formation time of the resonance to the larger formation time of the
   * incoming particles, if it is larger than the execution time; execution time
   * is otherwise taken to be the formation time */
  const double t0 = incoming_particles_[0].formation_time();
  const double t1 = incoming_particles_[1].formation_time();

  const size_t index_tmax = (t0 > t1) ? 0 : 1;
  const double sc =
      incoming_particles_[index_tmax].cross_section_scaling_factor();
  if (t0 > time_of_execution_ || t1 > time_of_execution_) {
    outgoing_particles_[0].set_formation_time(std::max(t0, t1));
    outgoing_particles_[0].set_cross_section_scaling_factor(sc);
  } else {
    outgoing_particles_[0].set_formation_time(time_of_execution_);
  }
  log.debug("Momentum of the new particle: ",
            outgoing_particles_[0].momentum());
}

/* This function will generate outgoing particles in CM frame
 * from a hard process. */
void ScatterAction::string_excitation_pythia() {
  assert(incoming_particles_.size() == 2);
  const auto &log = logger<LogArea::Pythia>();

  const double sqrts = sqrt_s();
  const PdgCode pdg1 = incoming_particles_[0].type().pdgcode();
  const PdgCode pdg2 = incoming_particles_[1].type().pdgcode();

  // Disable doubleing point exception trap for Pythia
  {
    DisableFloatTraps guard;
    /* set all necessary parameters for Pythia
     * Create Pythia object */
    log.debug("Creating Pythia object.");
    static /*thread_local (see #3075)*/ Pythia8::Pythia pythia(PYTHIA_XML_DIR,
                                                               false);
    /* select only inelastic events: */
    // pythia.readString("SoftQCD:inelastic = on");
    pythia.readString("SoftQCD:nonDiffractive = on");
    pythia.readString("MultipartonInteractions:pTmin = 1.5");
    /* suppress unnecessary output */
    pythia.readString("Print:quiet = on");
    /* Create output of the Pythia particle list */
    // pythia.readString("Init:showAllParticleData = on");
    /* No resonance decays, since the resonances will be handled by SMASH */
    pythia.readString("HadronLevel:Decay = off");
    /* manually set the parton distribution function */
    pythia.readString("PDF:pSet = 13");
    pythia.readString("PDF:pSetB = 13");
    pythia.readString("PDF:piSet = 1");
    pythia.readString("PDF:piSetB = 1");
    /* set particle masses and widths in PYTHIA
     * to be same with those in SMASH */
    for (auto &ptype : ParticleType::list_all()) {
      int pdgid = ptype.pdgcode().get_decimal();
      double mass_pole = ptype.mass();
      double width_pole = ptype.width_at_pole();
      /* check if the particle species is in PYTHIA */
      if (pythia.particleData.isParticle(pdgid)) {
        /* set mass and width in PYTHIA */
        pythia.particleData.m0(pdgid, mass_pole);
        pythia.particleData.mWidth(pdgid, width_pole);
      }
    }
    /* make energy-momentum conservation in PYTHIA more precise */
    pythia.readString("Check:epTolErr = 1e-6");
    pythia.readString("Check:epTolWarn = 1e-8");
    /* Set the random seed of the Pythia Random Number Generator.
     * Please note: Here we use a random number generated by the
     * SMASH, since every call of pythia.init should produce
     * different events. */
    pythia.readString("Random:setSeed = on");
    std::stringstream buffer1, buffer2, buffer3, buffer4;
    buffer1 << "Random:seed = " << Random::canonical();
    pythia.readString(buffer1.str());
    /* set the incoming particles */
    buffer2 << "Beams:idA = " << pdg1;
    pythia.readString(buffer2.str());
    log.debug("First particle in string excitation: ", pdg1);
    buffer3 << "Beams:idB = " << pdg2;
    log.debug("Second particle in string excitation: ", pdg2);
    pythia.readString(buffer3.str());
    buffer4 << "Beams:eCM = " << sqrts;
    pythia.readString(buffer4.str());
    log.debug("Pythia call with eCM = ", buffer4.str());
    /* Initialize Pythia. */
    const bool pythia_initialized = pythia.init();
    if (!pythia_initialized) {
      throw std::runtime_error("Pythia failed to initialize.");
    }
    /* Short notation for Pythia event */
    Pythia8::Event &event = pythia.event;
    bool final_state_success = false;
    while (!final_state_success) {
      final_state_success = pythia.next();
    }
    ParticleList new_intermediate_particles;
    for (int i = 0; i < event.size(); i++) {
      if (event[i].isFinal()) {
        if (event[i].isHadron()) {
          int pythia_id = event[i].id();
          log.debug("PDG ID from Pythia:", pythia_id);
          /* K_short and K_long need to be converted to K0
           * since SMASH only knows K0 */
          if (pythia_id == 310 || pythia_id == 130) {
            const double prob = Random::uniform(0., 1.);
            if (prob <= 0.5) {
              pythia_id = 311;
            } else {
              pythia_id = -311;
            }
          }
          const std::string s = std::to_string(pythia_id);
          PdgCode pythia_code(s);
          ParticleData new_particle(ParticleType::find(pythia_code));
          FourVector momentum;
          momentum.set_x0(event[i].e());
          momentum.set_x1(event[i].px());
          momentum.set_x2(event[i].py());
          momentum.set_x3(event[i].pz());
          new_particle.set_4momentum(momentum);
          log.debug("4-momentum from Pythia: ", momentum);
          new_intermediate_particles.push_back(new_particle);
        }
      }
    }
    /*
     * sort new_intermediate_particles according to z-Momentum
     */
    std::sort(
        new_intermediate_particles.begin(), new_intermediate_particles.end(),
        [&](ParticleData i, ParticleData j) {
          return std::abs(i.momentum().x3()) > std::abs(j.momentum().x3());
        });
    for (ParticleData data : new_intermediate_particles) {
      log.debug("Particle momenta after sorting: ", data.momentum());
      /* The hadrons are not immediately formed, currently a formation time of
       * 1 fm is universally applied and cross section is reduced to zero and
       * to a fraction corresponding to the valence quark content. Hadrons
       * containing a valence quark are determined by highest z-momentum. */
      log.debug("The formation time is: ", string_formation_time_, "fm/c.");
      /* Additional suppression factor to mimic coherence taken as 0.7
       * from UrQMD (CTParam(59) */
      const double suppression_factor = 0.7;
      if (incoming_particles_[0].is_baryon() ||
          incoming_particles_[1].is_baryon()) {
        if (data == 0) {
          data.set_cross_section_scaling_factor(suppression_factor * 0.66);
        } else if (data == 1) {
          data.set_cross_section_scaling_factor(suppression_factor * 0.34);
        } else {
          data.set_cross_section_scaling_factor(suppression_factor * 0.0);
        }
      } else {
        if (data == 0 || data == 1) {
          data.set_cross_section_scaling_factor(suppression_factor * 0.50);
        } else {
          data.set_cross_section_scaling_factor(suppression_factor * 0.0);
        }
      }
      ThreeVector v_calc =
          (data.momentum().LorentzBoost(-1.0 * beta_cm())).velocity();
      // Set formation time: actual time of collision + time to form the
      // particle
      double gamma_factor = 1.0 / std::sqrt(1 - (v_calc).sqr());
      data.set_formation_time(string_formation_time_ * gamma_factor +
                              time_of_execution_);
      outgoing_particles_.push_back(data);
    }
    /* If the incoming particles already were unformed, the formation
     * times and cross section scaling factors need to be adjusted */
    const double tform_in = std::max(incoming_particles_[0].formation_time(),
                                     incoming_particles_[1].formation_time());
    if (tform_in > time_of_execution_) {
      const double fin =
          (incoming_particles_[0].formation_time() >
           incoming_particles_[1].formation_time())
              ? incoming_particles_[0].cross_section_scaling_factor()
              : incoming_particles_[1].cross_section_scaling_factor();
      for (size_t i = 0; i < outgoing_particles_.size(); i++) {
        const double tform_out = outgoing_particles_[i].formation_time();
        const double fout =
            outgoing_particles_[i].cross_section_scaling_factor();
        outgoing_particles_[i].set_cross_section_scaling_factor(fin * fout);
        /* If the unformed incoming particles' formation time is larger than
         * the current outgoing particle's formation time, then the latter
         * is overwritten by the former*/
        if (tform_in > tform_out) {
          outgoing_particles_[i].set_formation_time(tform_in);
        }
      }
    }
    /* Check momentum difference for debugging */
    FourVector out_mom;
    for (ParticleData data : outgoing_particles_) {
      out_mom += data.momentum();
    }
    log.debug("Incoming momenta string:", total_momentum());
    log.debug("Outgoing momenta string:", out_mom);
  }
}

/* This function will generate outgoing particles in CM frame
 * from a hard process.
 * The way to excite strings is based on the UrQMD model */
void ScatterAction::string_excitation_soft() {
  assert(incoming_particles_.size() == 2);
  const auto &log = logger<LogArea::Pythia>();
  // Disable doubleing point exception trap for Pythia
  {
    DisableFloatTraps guard;
    /* initialize the string_process_ object for this particular collision */
    string_process_->init(incoming_particles_, time_of_execution_, gamma_cm());
    /* implement collision */
    bool success = false;

    /* subprocess selection */
    int iproc = -1;
    double r_xsec = string_sub_cross_sections_sum_[4] * Random::uniform(0., 1.);
    for (int i = 0; i < 4; i++) {
      if ((r_xsec >= string_sub_cross_sections_sum_[i]) &&
          (r_xsec < string_sub_cross_sections_sum_[i + 1])) {
        iproc = i;
        break;
      }
    }
    if (iproc == -1) {
      throw std::runtime_error("soft string subprocess is not specified.");
    }

    int ntry = 0;
    const int ntry_max = 10000;
    while (!success && ntry < ntry_max) {
      ntry++;
      switch (iproc) {
        case 0:
          /* single diffractive to A+X */
          success = string_process_->next_SDiff(true);
          break;
        case 1:
          /* single diffractive to X+B */
          success = string_process_->next_SDiff(false);
          break;
        case 2:
          /* double diffractive */
          success = string_process_->next_DDiff();
          break;
        case 3:
          /* soft non-diffractive */
          success = string_process_->next_NDiffSoft();
          break;
        default:
          success = false;
      }
    }
    if (ntry == ntry_max) {
      throw std::runtime_error("too many tries in string_excitation_soft().");
    }
    outgoing_particles_ = string_process_->get_final_state();
    /* If the incoming particles already were unformed, the formation
     * times and cross section scaling factors need to be adjusted */
    const double tform_in = std::max(incoming_particles_[0].formation_time(),
                                     incoming_particles_[1].formation_time());
    if (tform_in > time_of_execution_) {
      const double fin =
          (incoming_particles_[0].formation_time() >
           incoming_particles_[1].formation_time())
              ? incoming_particles_[0].cross_section_scaling_factor()
              : incoming_particles_[1].cross_section_scaling_factor();
      for (size_t i = 0; i < outgoing_particles_.size(); i++) {
        const double tform_out = outgoing_particles_[i].formation_time();
        const double fout =
            outgoing_particles_[i].cross_section_scaling_factor();
        outgoing_particles_[i].set_cross_section_scaling_factor(fin * fout);
        /* If the unformed incoming particles' formation time is larger than
         * the current outgoing particle's formation time, then the latter
         * is overwritten by the former*/
        if (tform_in > tform_out) {
          outgoing_particles_[i].set_formation_time(tform_in);
        }
      }
    }
    /* Check momentum difference for debugging */
    FourVector out_mom;
    for (ParticleData data : outgoing_particles_) {
      out_mom += data.momentum();
    }
    log.debug("Incoming momenta string:", total_momentum());
    log.debug("Outgoing momenta string:", out_mom);
  }
}

void ScatterAction::format_debug_output(std::ostream &out) const {
  out << "Scatter of " << incoming_particles_;
  if (outgoing_particles_.empty()) {
    out << " (not performed)";
  } else {
    out << " to " << outgoing_particles_;
  }
}

}  // namespace smash
