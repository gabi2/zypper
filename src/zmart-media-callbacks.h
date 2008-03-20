/*---------------------------------------------------------------------\
|                          ____ _   __ __ ___                          |
|                         |__  / \ / / . \ . \                         |
|                           / / \ V /|  _/  _/                         |
|                          / /__ | | | | | |                           |
|                         /_____||_| |_| |_|                           |
|                                                                      |
\---------------------------------------------------------------------*/

#ifndef ZMART_MEDIA_CALLBACKS_H
#define ZMART_MEDIA_CALLBACKS_H

#include <stdlib.h>
#include <iostream>

#include <zypp/base/Logger.h>
#include <zypp/ZYppCallbacks.h>
#include <zypp/Pathname.h>
#include <zypp/KeyRing.h>
#include <zypp/Digest.h>
#include <zypp/Url.h>
#include <zypp/Source.h>

#include "AliveCursor.h"
#include "zypper-callbacks.h"

using zypp::media::MediaChangeReport;
using zypp::media::DownloadProgressReport;

///////////////////////////////////////////////////////////////////
namespace ZmartRecipients
{

  struct MediaChangeReportReceiver : public zypp::callback::ReceiveReport<MediaChangeReport>
  {
    virtual MediaChangeReport::Action requestMedia( zypp::Source_Ref source, unsigned mediumNr, MediaChangeReport::Error error, cbstring description )
    {
      // TranslatorExplanation don't translate letters 'y' and 'n' for now
      std::string request = boost::str(boost::format(
          _("Please insert media [%s] # %d and type 'y' to continue or 'n' to cancel the operation."))
          % description % mediumNr);
      if (read_bool_answer(request, false))
        return MediaChangeReport::RETRY; 
      else
        return MediaChangeReport::ABORT; 
    }
  };

    // progress for downloading a file
  struct DownloadProgressReportReceiver : public zypp::callback::ReceiveReport<zypp::media::DownloadProgressReport>
  {
    virtual void start( cbUrl file, zypp::Pathname localfile )
    {
      cerr_v << "Downloading: "
	     << file << " to " << localfile << std::endl;
    }

    virtual bool progress(int value, cbUrl /*file*/)
    {
      display_progress ("Downloading", value);
      return true;
    }

    virtual DownloadProgressReport::Action problem( cbUrl /*file*/, DownloadProgressReport::Error error, cbstring description )
    {
      display_done ();
      display_error (error, description);
      return DownloadProgressReport::ABORT;
    }

    virtual void finish( cbUrl /*file*/, Error error, cbstring konreason )
    {
      display_done ();
      display_error (error, konreason);
    }
  };
  
    ///////////////////////////////////////////////////////////////////
}; // namespace ZmartRecipients
///////////////////////////////////////////////////////////////////

class MediaCallbacks {

  private:
    ZmartRecipients::MediaChangeReportReceiver _mediaChangeReport;
    ZmartRecipients::DownloadProgressReportReceiver _mediaDownloadReport;
  public:
    MediaCallbacks()
    {
      _mediaChangeReport.connect();
      _mediaDownloadReport.connect();
    }

    ~MediaCallbacks()
    {
      _mediaChangeReport.disconnect();
      _mediaDownloadReport.disconnect();
    }
};

#endif 
// Local Variables:
// mode: c++
// c-basic-offset: 2
// End: