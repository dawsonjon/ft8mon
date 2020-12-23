#include "snd.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <assert.h>
#include <vector>
#include <string>
#include <string.h>
#include <math.h>
#include "util.h"
#include "fft.h"
#include <portaudio.h>
#include <ctype.h>

void
snd_init()
{
  static int inited = 0;

  if(!inited){
    inited = 1;
    int err = Pa_Initialize();
    if(err != paNoError){
      fprintf(stderr, "Pa_Initialize() failed\n");
      exit(1);
    }
  }
}

extern "C" void
airspy_list()
{
#ifdef AIRSPYHF
  int ndev = airspyhf_list_devices(0, 0);
  if(ndev > 0){
    uint64_t serials[20];
    if(ndev > 20)
      ndev = 20;
    airspyhf_list_devices(serials, ndev);
    for(int unit = 0; unit < ndev; unit++){
      airspyhf_device_t *dev = 0;
      if(airspyhf_open_sn(&dev, serials[unit]) == AIRSPYHF_SUCCESS){
        airspyhf_read_partid_serialno_t read_partid_serialno;
        airspyhf_board_partid_serialno_read(dev, &read_partid_serialno);
        printf("Airspy HF+ serial %08X%08X\n",
               read_partid_serialno.serial_no[0],
               read_partid_serialno.serial_no[1]);
      } else {
        fprintf(stderr, "could not open airspyhf unit %d\n", unit);
      }
    }
  }
#endif
}

//
// print a list of sound devices
//
void
snd_list()
{
  snd_init();
  int n = Pa_GetDeviceCount();
  printf("%d sound devices:\n", n);
  for(int di = 0; di < n; di++){
    const PaDeviceInfo *info = Pa_GetDeviceInfo(di);
    if(info == 0)
      continue;
    printf("%d: %s %d/%d ",
           di,
           info->name,
           info->maxInputChannels,
           info->maxOutputChannels);

    PaStreamParameters ip;
    memset(&ip, 0, sizeof(ip));
    ip.device = di;
    ip.channelCount = 1;
    ip.sampleFormat = paInt16;
    ip.suggestedLatency = 0;
    ip.hostApiSpecificStreamInfo = 0;
    int rates[] = { 6000, 8000, 11025, 12000, 16000, 22050, 44100, 48000, 0 };
    for(int ri = 0; rates[ri]; ri++){
      PaError err = Pa_IsFormatSupported(&ip, 0, rates[ri]);
      if(err == paNoError){
        printf("%d ", rates[ri]);
      }
    }

    // printf("-- %f %f ", info->defaultLowOutputLatency, info->defaultHighOutputLatency);

    printf("\n");
  }

  airspy_list();
}

//
// print avg and peak each second.
//
void
SoundIn::levels()
{
  double max = 0;
  double sum = 0;
  int n = 0;
  double last_t = now();

  while(1){
    double dummy;
    std::vector<double> buf = get(rate(), dummy, 0);
    if(buf.size() == 0)
      usleep(100*1000);
    for(int i = 0; i < buf.size(); i++){
      sum += fabs(buf[i]);
      n += 1;
      if(fabs(buf[i]) > max){
        max = fabs(buf[i]);
      }
      if(n >= rate()){
        printf("avg=%.3f peak=%.3f rate=%.1f\n", sum / n, max, n / (now() - last_t));
        n = 0;
        sum = 0;
        max = 0;
        last_t = now();
      }
    }
  }
}

//
// generic open.
// wanted_rate can be -1.
//
SoundIn *
SoundIn::open(std::string card, std::string chan, int wanted_rate)
{
  assert(card.size() > 0);
  SoundIn *sin;

  if(isdigit(card[0])){
    sin = new CardSoundIn(atoi(card.c_str()), atoi(chan.c_str()), wanted_rate);
  } else if(card == "file"){
    sin = new FileSoundIn(chan, wanted_rate);
#ifdef AIRSPYHF
  } else if(card == "airspy"){
    sin = new AirspySoundIn(chan, wanted_rate);
#endif
  } else {
    fprintf(stderr, "SoundIn::open(%s, %s): type not recognized\n", card.c_str(), chan.c_str());
    exit(1);
  }

  return sin;
}
  
int
CardSoundIn::cb(const void *input,
            void *output,
            unsigned long frameCount,
            const struct PaStreamCallbackTimeInfo *timeInfo,
            unsigned long statusFlags, // PaStreamCallbackFlags
            void *userData)
{
  CardSoundIn *sin = (CardSoundIn *) userData;
  const short int *buf = (const short int *) input;

  if(statusFlags != 0){
    // 2 is paInputOverflow
    fprintf(stderr, "CardSoundIn::cb statusFlags 0x%x frameCount %d\n",
            (int)statusFlags, (int)frameCount);
  }

  for(int i = 0; i < frameCount; i++){
    if(((sin->wi_ + 1) % sin->n_) != sin->ri_){
      sin->buf_[sin->wi_] = buf[i*sin->channels_ + sin->chan_];
      sin->wi_ = (sin->wi_ + 1) % sin->n_;
    } else {
      fprintf(stderr, "CardSoundIn::cb buf_ overflow\n");
      break;
    }
  }

  sin->time_ = timeInfo->inputBufferAdcTime + frameCount * (1.0 / sin->rate_);

  return 0;
}

CardSoundIn::CardSoundIn(int card, int chan, int wanted_rate)
{
  card_ = card;
  chan_ = chan;
  assert(chan_ >= 0 && chan_ <= 1);
  time_ = -1;
  rate_ = wanted_rate;
}

//
// read a bunch of recent sound samples.
// read up to n samples, no more.
// return immediately with whatever samples exist,
// perhaps fewer than n.
// return UNIX time of first sample in t0.
// if latest==1, return (up to) the most recent n samples
// and discard any earlier samples.
//
std::vector<double>
CardSoundIn::get(int n, double &t0, int latest)
{
  std::vector<double> v;

  if(time_ < 0 && wi_ == ri_){
    // no input has ever arrived.
    t0 = -1;
    return v;
  }

  if(latest){
    while(((wi_ + n_ - ri_) % n_) > n){
      ri_ = (ri_ + 1) % n_;
    }
  }

  // calculate time of first sample in buf_.
  // XXX there's a race here with cb().
  t0 = time_ + dt_; // UNIX time of last sample in buf_.
  if(wi_ > ri_){
    t0 -= (wi_ - ri_) * (1.0 / rate_);
  } else {
    t0 -= ((wi_ + n_) - ri_) * (1.0 / rate_);
  }

  while(v.size() < n){
    if(ri_ == wi_){
      break;
    }
    short x = buf_[ri_];
    v.push_back(x / 32767.0);
    ri_ = (ri_ + 1) % n_;
  }

  return v;
}

void
CardSoundIn::start()
{
  snd_init();

  if(rate_ == -1){
#ifdef __linux__
    // RIGblaster only supports 44100 and 48000.
    rate_ = 48000;
#else
    rate_ = 12000;
#endif
  }

#ifdef __FreeBSD__
  // must read both, otherwise FreeBSD mixes them.
  channels_ = 2;
#else
  if(chan_ == 0){
    channels_ = 1;
  } else {
    channels_ = 2;
  }
#endif

  PaStreamParameters ip;
  memset(&ip, 0, sizeof(ip));
  ip.device = card_;
  ip.channelCount = channels_;
  ip.sampleFormat = paInt16;
  ip.hostApiSpecificStreamInfo = 0;

  // don't set latency to zero; this causes problems on Linux.
  ip.suggestedLatency = Pa_GetDeviceInfo(card_)->defaultLowInputLatency;
  
  PaStream *str = 0;
  PaError err = Pa_OpenStream(&str,
                              &ip,
                              0,
                              rate_,
#ifdef __FreeBSD__
                              128, // framesPerBuffer
#else
                              0, // framesPerBuffer
#endif
                              0,
                              cb,
                              (void*) this);
  if(err != paNoError){
    fprintf(stderr, "Pa_OpenStream(card=%d,rate=%d) failed for input: %s\n",
            card_, rate_, Pa_GetErrorText(err));
    exit(1);
  }

  // allocate a 30-second circular buffer
  n_ = rate_ * 30;
  buf_ = (short *) malloc(sizeof(short) * n_);
  assert(buf_);
  wi_ = 0;
  ri_ = 0;

  err = Pa_StartStream(str);
  if(err != paNoError){
    fprintf(stderr, "Pa_StartStream failed\n");
    exit(1);
  }

  dt_ = now() - Pa_GetStreamTime(str);

}

#ifdef AIRSPYHF
//
// chan argument is serial[,megahertz].
// e.g. 3B52EB5DAC35398D,14.074
// or -,megahertz
// or just serial #
//
AirspySoundIn::AirspySoundIn(std::string chan, int wanted_rate)
{
  device_ = 0;
  //hz_ = 1000000.0 * atof(chan.c_str());
  hz_ = 10 * 1000 * 1000;
  air_rate_ = 192 * 1000;;
  time_ = -1;
  count_ = 0;
  strcpy(hostname_, "???");
  gethostname(hostname_, sizeof(hostname_));
  hostname_[sizeof(hostname_)-1] = '\0';

  if(wanted_rate == -1){
    rate_ = 12000;
  } else {
    rate_ = wanted_rate;
  }

  int comma = chan.find(",");
  if(comma >= 0){
    hz_ = atof(chan.c_str() + comma + 1) * 1000000.0;
  }

  if(comma == 0 || chan.size() < 1 || chan[0] == '-'){
    if(airspyhf_open(&device_) != AIRSPYHF_SUCCESS ) {
      fprintf(stderr, "airspyhf_open() failed\n");
      exit(1);
    }
  } else {
    // open by Airspy HF+ serial number.
    uint64_t serial = strtoull(chan.c_str(), 0, 16);
    if(airspyhf_open_sn(&device_, serial) != AIRSPYHF_SUCCESS ) {
      fprintf(stderr, "airspyhf_open_sn(%llx) failed\n", serial);
      exit(1);
    }
  }
  
  serial_ = get_serial();

  if (airspyhf_set_samplerate(device_, air_rate_) != AIRSPYHF_SUCCESS) {
    fprintf(stderr, "airspyhf_set_samplerate(%d) failed\n", air_rate_);
    exit(1);
  }

#if 0
  if (airspyhf_set_hf_lna(device_, 1) != AIRSPYHF_SUCCESS) {
    fprintf(stderr, "airspyhf_set_hf_lna(1) failed\n");
    exit(1);
  }
#endif

#if 0
  if (airspyhf_set_hf_att(device_, 5) != AIRSPYHF_SUCCESS) {
    fprintf(stderr, "airspyhf_set_hf_att(5) failed\n");
    exit(1);
  }
#endif

  // allocate a 60-second circular buffer
  n_ = rate_ * 60;
  buf_ = (std::complex<double> *) malloc(sizeof(std::complex<double>) * n_);
  assert(buf_);
  wi_ = 0;
  ri_ = 0;

  // Liquid DSP filter + resampler to convert 192000 to 6000.
  // firdecim?
  // iirdecim?
  // msresamp?
  // msresamp2?
  // resamp?
  // resamp2?
  int h_len = estimate_req_filter_len(0.01, 60.0);
  float h[h_len];
  double cutoff = (rate_ / (double) air_rate_) / 2.0;
  liquid_firdes_kaiser(h_len,
                       cutoff,
                       60.0,
                       0.0,
                       h);
  assert((air_rate_ % rate_) == 0);
  filter_ = firfilt_crcf_create(h, h_len);
}

unsigned long long
AirspySoundIn::get_serial()
{
  airspyhf_read_partid_serialno_t sn;
  airspyhf_board_partid_serialno_read(device_, &sn);
  unsigned long long x;
  x = ((unsigned long long) sn.serial_no[0]) << 32;
  x |= (unsigned long long) sn.serial_no[1];
  return x;
}

int
AirspySoundIn::set_freq(int hz)
{
  if(hz > 31000000 && hz < 60000000){
    // Airspy HF+ specifications say 0.5kHz .. 31 MHz and 60..260 MHz.
    // Thus not the 6-meter band.
    fprintf(stderr, "airspy %llx: unsupported frequency %d\n", serial_, hz);
  }

  int ret = airspyhf_set_freq(device_, hz);
  if(ret != AIRSPYHF_SUCCESS){
    fprintf(stderr, "airspyhf_set_freq(%d) failed.\n", hz);
    exit(1);
  }
  hz_ = hz;
  
  return hz;
}

void
AirspySoundIn::start()
{
  int ret = airspyhf_start(device_, cb1, this);
  if(ret != AIRSPYHF_SUCCESS){
    fprintf(stderr, "airspyhf_start() failed.\n");
    exit(1);
  }

  set_freq(hz_);
}

int
AirspySoundIn::cb1(airspyhf_transfer_t *transfer)
{
  AirspySoundIn *sin = (AirspySoundIn *) transfer->ctx;
  return sin->cb2(transfer);
}

int
AirspySoundIn::cb2(airspyhf_transfer_t *transfer)
{
  // transfer->sample_count
  // transfer->samples
  // transfer->ctx
  // transfer->dropped_samples
  // each sample is an I/Q pair of 32-bit floats

  if(transfer->dropped_samples){
    fprintf(stderr, "airspy %s %llx (%.3f MHz) dropped_samples %d, sample_count %d\n",
            hostname_,
            serial_,
            hz_ / 1000000.0,
            (int)transfer->dropped_samples,
            (int)transfer->sample_count);
  }

  time_ = now();

  airspyhf_complex_float_t *buf = transfer->samples;
  for(int i = 0; i < transfer->sample_count + transfer->dropped_samples; i++){
    // low-pass filter, preparatory to rate reduction.
    liquid_float_complex x, y;
    if(i < transfer->sample_count){
      x.real = buf[i].re;
      x.imag = buf[i].im;
    } else {
      x.real = x.imag = 0;
    }
    firfilt_crcf_push(filter_, x);
    firfilt_crcf_execute(filter_, &y);

    if((count_ % (air_rate_ / rate_)) == 0){
      if(((wi_ + 1) % n_) != ri_){
        // XXX what is the range of buf[i].re? 0..1?
        buf_[wi_] = std::complex<double>(y.real, y.imag);
        wi_ = (wi_ + 1) % n_;
      } else {
#if 0
        fprintf(stderr, "AirspySoundIn::cb buf_ overflow, serial=%llx\n",
                serial_);
#endif
        break;
      }
    }

    count_ += 1;
  }

  return 0;
}

std::vector<double>
vreal(std::vector<std::complex<double>> a)
{
  std::vector<double> b(a.size());
  for(int i = 0; i < a.size(); i++){
    b[i] = a[i].real();
  }
  return b;
}

std::vector<double>
vimag(std::vector<std::complex<double>> a)
{
  std::vector<double> b(a.size());
  for(int i = 0; i < a.size(); i++){
    b[i] = a[i].imag();
  }
  return b;
}

//
// convert I/Q to USB via the phasing method.
// uses FFTs over the whole of a[], so it's slow.
// and the results are crummy at the start and end.
//
std::vector<double>
iq2usb(std::vector<std::complex<double>> a)
{
  std::vector<double> ii = vreal(analytic(vreal(a), "snd::iq2usb_i"));
  std::vector<double> qq = vimag(analytic(vimag(a), "snd::iq2usb_q"));
  std::vector<double> ssb(ii.size());
  for(int i = 0; i < ii.size(); i++){
    ssb[i] = ii[i] - qq[i];
  }
  return ssb;
}

// UNIX time of first sample in t0.
std::vector<double>
AirspySoundIn::get(int n, double &t0, int latest)
{
  std::vector<double> nothing;

  if(time_ < 0 && wi_ == ri_){
    // no input has ever arrived.
    t0 = -1;
    return nothing;
  }

  if(latest){
    while(((wi_ + n_ - ri_) % n_) > n){
      ri_ = (ri_ + 1) % n_;
    }
  }

  // calculate time of first sample in buf_.
  // XXX there's a race here with cb().
  t0 = time_; // time of last sample in buf_.
  if(wi_ >= ri_){
    t0 -= (wi_ - ri_) * (1.0 / rate_);
  } else {
    t0 -= ((wi_ + n_) - ri_) * (1.0 / rate_);
  }

  std::vector<std::complex<double>> v1;
  while(v1.size() < n){
    if(ri_ == wi_){
      break;
    }
    v1.push_back(buf_[ri_]);
    ri_ = (ri_ + 1) % n_;
  }

  if(v1.size() < 2){
    // analytic() demands more than one sample.
    return vreal(v1);
  } else {
    // pad to increase chances that we can re-use an FFT plan.
    int olen = v1.size();
    int quantum;
    if(olen > rate() * 5){
      quantum = rate();
    } else if(olen > 1000){
      quantum = 1000;
    } else {
      quantum = 100;
    }
    int needed = quantum - (olen % quantum);
    if(needed != quantum){
      v1.resize(v1.size() + needed, 0.0);
    }
    std::vector<double> v2 = iq2usb(v1);
    if(v2.size() > olen){
      v2.resize(olen);
    }
    return v2;
  }
}
#endif

SoundOut::SoundOut(int card)
{
  card_ = card;
}

void
SoundOut::start()
{
  snd_init();

#ifdef __linux__
  // RIGblaster only supports 44100 and 48000.
  rate_ = 48000;
#else
  rate_ = 8000;
#endif

  PaStreamParameters op;
  memset(&op, 0, sizeof(op));
  op.device = card_;
  op.channelCount = 1;
  op.sampleFormat = paInt16;
  // latency must be the same as for input card.
  op.suggestedLatency = Pa_GetDeviceInfo(card_)->defaultLowInputLatency;
  op.hostApiSpecificStreamInfo = 0;
  
  str_ = 0;
  PaError err = Pa_OpenStream(&str_,
                              0,
                              &op,
                              rate_,
                              0, // framesPerBuffer
                              0,
                              0,
                              (void*) 0);
  if(err != paNoError){
    fprintf(stderr, "Pa_OpenStream(card=%d,rate=%d) failed for output: %s\n",
            card_, rate_, Pa_GetErrorText(err));
    exit(1);
  }

  err = Pa_StartStream(str_);
  if(err != paNoError){
    fprintf(stderr, "Pa_StartStream failed\n");
    exit(1);
  }

}

//
// Pa_WriteStream may block, but it does have some internal buffering.
// the amount seems to be controlled by suggestedLatency.
//
void
SoundOut::write(const std::vector<short int> &v)
{
  PaError err = Pa_WriteStream(str_, v.data(), v.size());
  if(err != paNoError && err != paOutputUnderflowed){
    fprintf(stderr, "Pa_WriteStream failed %d %s\n", err, Pa_GetErrorText(err));
    exit(1);
  }
}

void
SoundOut::write(const std::vector<double> &v)
{
  std::vector<short int> vv(v.size());
  for(int i = 0; i < v.size(); i++){
    if(v[i] > 1.0){
      fprintf(stderr, "SoundOut::write() oops %f\n", v[i]);
    }
    vv[i] = v[i] * 16380;
  }
  write(vv);
}

//
// functions for Python to call via ctypes.
//

extern "C" {
  void *ext_snd_open(const char *card, const char *chan, int wanted_rate);
  int ext_snd_read(void *, double *, int, double *);
  int ext_set_freq(void *, int);
}

void *
ext_snd_open(const char *card, const char *chan, int wanted_rate)
{
  SoundIn *sin = SoundIn::open(card, chan, wanted_rate);
  sin->start();
  return (void *) sin;
}

//
// reads up to maxout samples.
// non-blocking.
// *tm will be set to UNIX time of last sample in out[].
// return value is number of samples written to out[].
//
int
ext_snd_read(void *thing, double *out, int maxout, double *tm)
{
  SoundIn *sin = (SoundIn *) thing;
  double t0; // time of first sample.

  // XXX the "1" means return the latest maxout samples,
  // and discard samples older than that!
  std::vector<double> v = sin->get(maxout, t0, 1);

  assert(v.size() <= maxout);
  for(int i = 0; i < maxout && i < v.size(); i++){
    out[i] = v[i];
  }
  *tm = t0 + v.size() * (1.0 / sin->rate()); // time of last sample.
  return v.size();
}

int
ext_set_freq(void *thing, int hz)
{
  SoundIn *sin = (SoundIn *) thing;
  int x = sin->set_freq(hz);
  return x;
}
