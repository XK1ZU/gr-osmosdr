/* -*- c++ -*- */
/*
 * Copyright 2013 Dimitri Stolnikov <horiz0n@gmx.net>
 * Copyright 2014 Hoernchen <la@tfc-server.de>
 *
 * GNU Radio is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3, or (at your option)
 * any later version.
 *
 * GNU Radio is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with GNU Radio; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street,
 * Boston, MA 02110-1301, USA.
 */

/*
 * config.h is generated by configure.  It contains the results
 * of probing for features, options etc.  It should be the first
 * file included in your .cc file.
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdexcept>
#include <iostream>
#include <algorithm>

#include <boost/assign.hpp>
#include <boost/format.hpp>
#include <boost/detail/endian.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/thread/thread.hpp>

#include <gnuradio/io_signature.h>

#include "pciesdr_source_c.h"

#include "arg_helpers.h"

using namespace boost::assign;

static long gcd(long a, long b)
{
  if (a == 0)
    return b;
  else if (b == 0)
    return a;

  if (a < b)
    return gcd(a, b % a);
  else
    return gcd(b, a % b);
}

static inline MultiSDRState *pciesdr_open(const char *args)
{
  return msdr_open(args);
}

static inline void pciesdr_close(MultiSDRState *_dev)
{
  msdr_close(_dev);
}

static inline int pciesdr_stop_rx(MultiSDRState *_dev)
{
  return msdr_stop(_dev);
}

bool pciesdr_source_c::_running = 0;
boost::mutex pciesdr_source_c::_running_mutex;

pciesdr_source_c_sptr make_pciesdr_source_c (const std::string & args)
{
  return gnuradio::get_initial_sptr(new pciesdr_source_c (args));
}

/*
 * Specify constraints on number of input and output streams.
 * This info is used to construct the input and output signatures
 * (2nd & 3rd args to gr::block's constructor).  The input and
 * output signatures are used by the runtime system to
 * check that a valid number and type of inputs and outputs
 * are connected to this block.  In this case, we accept
 * only 0 input and 1 output.
 */
static const int MIN_IN = 0;   // mininum number of input streams
static const int MAX_IN = 0;   // maximum number of input streams
static const int MIN_OUT = 1;  // minimum number of output streams
static const int MAX_OUT = 1;  // maximum number of output streams

/*
 * The private constructor
 */
pciesdr_source_c::pciesdr_source_c (const std::string &args)
  : gr::sync_block ("pciesdr_source_c",
        gr::io_signature::make(MIN_IN, MAX_IN, sizeof (gr_complex)),
        gr::io_signature::make(MIN_OUT, MAX_OUT, sizeof (gr_complex))),
    _dev(NULL),
    _sample_rate(0),
    _center_freq(0),
    _freq_corr(false),
    _auto_gain(false),
    _amp_gain(0),
    _vga_gain(0),
    _bandwidth(0)
{

  int chan = 0;
  int rf_port = 0;
  std::string pciesdr_args;
  dict_t dict = params_to_dict(args);

  if (dict.count("args") && dict["args"].length() > 0) {
    pciesdr_args = dict["args"];
    // remove last bracket
    pciesdr_args = pciesdr_args.substr(0, pciesdr_args.find_last_of(']'));
  }

  timestamp_rx = 0;

  _dev = pciesdr_open(pciesdr_args.c_str());

  if (_dev == NULL) {
    throw std::runtime_error("PCIESDR creating failed, device ");
  }
  
  // prefil startup parameters
  msdr_set_default_start_params(_dev, &StartParams);

  StartParams.interface_type = SDR_INTERFACE_RF; /* RF interface */
  StartParams.sync_source = SDR_SYNC_NONE; /* no time synchronisation */
  StartParams.clock_source = SDR_CLOCK_INTERNAL; /* internal clock, using PPS to correct it */

  StartParams.rx_sample_fmt = SDR_SAMPLE_FMT_CF32; /* complex float32 */
  StartParams.rx_sample_hw_fmt = SDR_SAMPLE_HW_FMT_AUTO; /* choose best format fitting the bandwidth */

  StartParams.sample_rate_num[rf_port] = 1.5e6;
  StartParams.sample_rate_den[rf_port] = 1;
  StartParams.tx_freq[chan] = 1500e6;
  StartParams.rx_freq[chan] = 1500e6;

  StartParams.rx_channel_count = 1;
  StartParams.tx_channel_count = 1;
  StartParams.rx_gain[chan] = 40;
  StartParams.rx_bandwidth[chan] = 1e4;
  StartParams.rf_port_count = 1;
  StartParams.tx_port_channel_count[rf_port] = 1;
  StartParams.rx_port_channel_count[rf_port] = 1;
  /* if != 0, set a custom DMA buffer configuration. Otherwise the default is 150 buffers per 10 ms */
  StartParams.dma_buffer_count = 0;
  StartParams.dma_buffer_len = 1000; /* in samples */

  set_center_freq((get_freq_range().start() + get_freq_range().stop()) / 2.0 );
  set_sample_rate(get_sample_rates().start());
  set_bandwidth(0);

  set_gain(0); /* disable AMP gain stage by default to protect full sprectrum pre-amp from physical damage */

  //set_if_gain( 16 ); /* preset to a reasonable default (non-GRC use case) */

  {
    boost::mutex::scoped_lock lock(_running_mutex);

    _running = 0;
  }

}

/*
 * Our virtual destructor.
 */
pciesdr_source_c::~pciesdr_source_c ()
{
  if (_dev) {
    pciesdr_close(_dev);
  }
}

int pciesdr_source_c::pciesdr_set_freq(MultiSDRState *_dev, uint64_t corr_freq)
{
  if (corr_freq < 70e6 || corr_freq > 6000e6) {
    std::cerr << "pciesdr_set_freq freq is out of range" << std::endl;
    return 1;
  }
  StartParams.rx_freq[0] = corr_freq;
  return 0;
}

int pciesdr_source_c::pciesdr_set_sample_rate(MultiSDRState *_dev, double rate)
{
  int chan = 0;

  if (rate < 400e3 || rate > 25e6) {
    std::cerr << "pciesdr_set_sample_rate: sample rate is out of range" << std::endl;
    return 1;
  }
  double integral = std::floor(rate);
  double frac = rate - integral;

  const long precision = 1000000000; // This is the accuracy

  long gcd_ = gcd(round(frac * precision), precision);

  long denominator = precision / gcd_;
  long numerator = round(frac * precision) / gcd_;
  
  StartParams.sample_rate_num[chan] = (int64_t)(integral * denominator + numerator);
  StartParams.sample_rate_den[chan] = denominator;

  return 0;
}

bool pciesdr_source_c::start()
{
  int ret;
  SDRStats stats;

  if (! _dev)
    return false;

  ret = msdr_start(_dev, &StartParams);
  if (ret) {
    std::cerr << "Failed to start RX streaming" << std::endl;
    return false;
  }

  ret = msdr_get_stats(_dev, &stats);
  if (ret) {
    std::cerr << "msdr_get_stats failed" << std::endl;
    return false;
  }

  timestamp_rx = 0;

  {
    boost::mutex::scoped_lock lock(_running_mutex);

    _running = 1;
  }

  return true;
}

bool pciesdr_source_c::stop()
{
  if (! _dev)
    return false;

  int ret = pciesdr_stop_rx(_dev);
  if (ret) {
    std::cerr << "Failed to stop RX streaming (" << ret << ")" << std::endl;
    return false;
  }

  {
    boost::mutex::scoped_lock lock(_running_mutex);

    _running = 0;
  }

  return true;
}

int pciesdr_source_c::work( int noutput_items,
                         gr_vector_const_void_star &input_items,
                         gr_vector_void_star &output_items )
{
  int chan = 0;
  int chan_count = 1;
  int rc;
  int i;
  SDRStats stats;
  int64_t timestamp_tmp = 0;
  sample_t *rx_samples_by_chan[SDR_MAX_CHANNELS];

  for (i = 0; i < chan_count; i++) {
  /*
   *TODO: check sample buffer address alignment (alignment should be 32B)
  */
    rx_samples_by_chan[i] = (sample_t*)output_items[i];
  }
  rc = msdr_read(_dev, &timestamp_tmp, (void**)rx_samples_by_chan, noutput_items, chan, 100); 
  if (rc < 0) {
    std::cerr << "Failed read from RX stream rc:" << rc << " noutput_items:" << noutput_items << std::endl;
    std::cerr << "timestamp_rx:" << timestamp_rx << " timestamp_tmp:" << timestamp_tmp << std::endl;
    if (msdr_get_stats(_dev, &stats)) {
      std::cerr << "Failed get_stats" << std::endl;
    } else {
      std::cerr << "tx_underflow_count:" << stats.tx_underflow_count << " rx_overflow_count:" << stats.rx_overflow_count << std::endl;
    }
    return 0;
  }
  timestamp_rx = timestamp_tmp;

  // Tell runtime system how many output items we produced.

  return rc;
}

std::vector<std::string> pciesdr_source_c::get_devices()
{
  std::vector<std::string> devices;
  std::string label;
  devices.push_back("dev0=/dev/sdr0");

  return devices;
}

size_t pciesdr_source_c::get_num_channels()
{
  return 1;
}

osmosdr::meta_range_t pciesdr_source_c::get_sample_rates()
{
  osmosdr::meta_range_t range;

  /* we only add integer rates here because of better phase noise performance.
   * the user is allowed to request arbitrary (fractional) rates within these
   * boundaries. */

  range += osmosdr::range_t(400e3, 20e6);

  return range;
}

double pciesdr_source_c::set_sample_rate( double rate )
{
  int ret;

  if (_dev) {
    ret = pciesdr_set_sample_rate( _dev, rate );
    if (!ret) {
      _sample_rate = rate;
      //set_bandwidth( 0.0 ); /* bandwidth of 0 means automatic filter selection */
    }
  }

  return get_sample_rate();
}

double pciesdr_source_c::get_sample_rate()
{
  return _sample_rate;
}

osmosdr::freq_range_t pciesdr_source_c::get_freq_range( size_t chan )
{
  osmosdr::freq_range_t range;

  range.push_back(osmosdr::range_t(70e6, 6000e6));

  return range;
}

double pciesdr_source_c::set_center_freq( double freq, size_t chan )
{
  int ret;

  #define APPLY_PPM_CORR(val, ppm) ((val) * (1.0 + (ppm) * 0.000001))

  if (_dev) {
    double corr_freq = APPLY_PPM_CORR( freq, _freq_corr );
    ret = pciesdr_set_freq( _dev, uint64_t(corr_freq) );
    if (!ret) {
      _center_freq = freq;
    }
  }

  return get_center_freq( chan );
}

double pciesdr_source_c::get_center_freq( size_t chan )
{
  return _center_freq;
}

double pciesdr_source_c::set_freq_corr( double ppm, size_t chan )
{
  _freq_corr = ppm;

  set_center_freq( _center_freq );

  return get_freq_corr( chan );
}

double pciesdr_source_c::get_freq_corr( size_t chan )
{
  return _freq_corr;
}

std::vector<std::string> pciesdr_source_c::get_gain_names( size_t chan )
{
  std::vector< std::string > names;

  names += "RF";
  names += "IF";

  return names;
}

osmosdr::gain_range_t pciesdr_source_c::get_gain_range( size_t chan )
{
  return get_gain_range( "RF", chan );
}

osmosdr::gain_range_t pciesdr_source_c::get_gain_range( const std::string & name, size_t chan )
{
  if ( "RF" == name ) {
    return osmosdr::gain_range_t(0, 60);
  }

  if ( "IF" == name ) {
    return osmosdr::gain_range_t(0, 60);
  }

  return osmosdr::gain_range_t();
}

bool pciesdr_source_c::set_gain_mode( bool automatic, size_t chan )
{
  _auto_gain = automatic;

  return get_gain_mode(chan);
}

bool pciesdr_source_c::get_gain_mode( size_t chan )
{
  return _auto_gain;
}

double pciesdr_source_c::set_gain( double gain, size_t chan )
{
  int ret = 0;

  StartParams.rx_gain[chan] = gain;

  {
    boost::mutex::scoped_lock lock(_running_mutex);

    if (_running) {
      ret = msdr_set_rx_gain(_dev, chan, StartParams.rx_gain[chan]);
    }
  }

  if (ret) {
    std::cerr << "Failed to set RX gain (" << ret << "), chan: " << chan << std::endl;
  }

  return get_gain(chan);
}

double pciesdr_source_c::set_gain( double gain, const std::string & name, size_t chan)
{
  if ( "RF" == name ) {
    return set_gain( gain, chan );
  }

  if ( "IF" == name ) {
    return set_if_gain( gain, chan );
  }

  return set_gain( gain, chan );
}

double pciesdr_source_c::get_gain( size_t chan )
{
  double gain = StartParams.rx_gain[chan];

  {
    boost::mutex::scoped_lock lock(_running_mutex);

    if (_running) {
      gain = msdr_get_rx_gain(_dev, chan);
    }
  }

  return gain;
}

double pciesdr_source_c::get_gain( const std::string & name, size_t chan )
{
  if ( "RF" == name ) {
    return get_gain( chan );
  }

  if ( "IF" == name ) {
    return get_gain( chan );
  }

  return get_gain( chan );
}

double pciesdr_source_c::set_if_gain( double gain, size_t chan )
{
  osmosdr::gain_range_t if_gains = get_gain_range( "IF", chan );

  return _vga_gain;
}

double pciesdr_source_c::set_bb_gain( double gain, size_t chan )
{
  return 0;
}

std::vector< std::string > pciesdr_source_c::get_antennas( size_t chan )
{
  std::vector< std::string > antennas;

  antennas += get_antenna( chan );

  return antennas;
}

std::string pciesdr_source_c::set_antenna( const std::string & antenna, size_t chan )
{
  return get_antenna( chan );
}

std::string pciesdr_source_c::get_antenna( size_t chan )
{
  return "TX/RX";
}

double pciesdr_source_c::set_bandwidth( double bandwidth, size_t chan )
{
  if (bandwidth == 0.0) /* bandwidth of 0 means automatic filter selection */
    bandwidth = _sample_rate * 0.75; /* select narrower filters to prevent aliasing */

  StartParams.rx_bandwidth[chan] = bandwidth;
  _bandwidth = get_bandwidth(chan);

  return _bandwidth;
}

double pciesdr_source_c::get_bandwidth( size_t chan )
{
  return StartParams.rx_bandwidth[chan];
}

osmosdr::freq_range_t pciesdr_source_c::get_bandwidth_range( size_t chan )
{
  osmosdr::freq_range_t bandwidths;

  // TODO: do this properly

  bandwidths += osmosdr::range_t(400e3, 20e6);

  return bandwidths;
}
