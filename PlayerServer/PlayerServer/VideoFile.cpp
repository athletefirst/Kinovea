/*
Copyright © Joan Charmant 2008-2009.
joan.charmant@gmail.com 
 
This file is part of Kinovea.

Kinovea is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License version 2 
as published by the Free Software Foundation.

Kinovea is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Kinovea. If not, see http://www.gnu.org/licenses/.

*/

#include <string.h>
#include <stdlib.h>
#include "VideoFile.h"
#include "VideoFileWriter.h"

using namespace System::Diagnostics;
using namespace System::Drawing;
using namespace System::Drawing::Drawing2D;
//using namespace System::Drawing::Imaging; // We can't use it because System::Drawing::Imaging::PixelFormat clashes with FFMpeg.
using namespace System::IO;
using namespace System::Runtime::InteropServices;

namespace Kinovea
{
namespace VideoFiles 
{

// --------------------------------------- Construction/Destruction
VideoFile::VideoFile()
{
	log->Debug("Constructing VideoFile.");

	// FFMpeg init.
	av_register_all();
	avcodec_init();
	avcodec_register_all();

	// Data init.
	m_bIsLoaded = false;
	m_iVideoStream = -1;
	m_iAudioStream = -1;
	m_iMetadataStream = -1;

	m_InfosVideo = gcnew InfosVideo();
	m_DefaultSettings = gcnew DefaultSettings();
	ResetInfosVideo();

	m_PrimarySelection = gcnew PrimarySelection();
	ResetPrimarySelection();

}
VideoFile::~VideoFile()
{
	if(m_bIsLoaded)
	{
		Unload();
	}
}
///<summary>
/// Finalizer. Attempt to free resources and perform other cleanup operations before the Object is reclaimed by garbage collection.
///</summary>
VideoFile::!VideoFile()
{
	if(m_bIsLoaded)
	{
		Unload();
	}
}

// --------------------------------------- Public Methods

/// <summary>
/// Set the default settings that will be used when loading the video.
/// </summary>
void VideoFile::SetDefaultSettings(int _AspectRatio, bool _bDeinterlaceByDefault)
{
	m_DefaultSettings->eAspectRatio = (AspectRatio)_AspectRatio;
	m_DefaultSettings->bDeinterlace = _bDeinterlaceByDefault;
}
/// <summary>
/// Loads specified file in the VideoFile instance.
/// Gets as much info on the video as possible.
/// </summary>
LoadResult VideoFile::Load(String^ _FilePath)
{

	LoadResult result = LoadResult::Success;

	int					iCurrentStream			= 0;
	int					iVideoStreamIndex		= -1;
	int					iMetadataStreamIndex	= -1;
	int					iAudioStreamIndex		= -1;

	AVFormatContext*	pFormatCtx; 
	AVCodecContext*		pCodecCtx;
	AVCodec*			pCodec;
	//AVCodecContext*		pAudioCodecCtx;
	//AVCodec*			pAudioCodec;

	m_FilePath = _FilePath;

	char*				cFilePath = static_cast<char *>(Marshal::StringToHGlobalAnsi(_FilePath).ToPointer());

	if(m_bIsLoaded) Unload();

	log->Debug("---------------------------------------------------");
	log->Debug("Entering : LoadMovie()");
	log->Debug("Input File						: " + Path::GetFileName(_FilePath));

	do
	{
		// 0. Récupérer la taille du fichier
		FileInfo^ fileInfo = gcnew FileInfo(_FilePath);
		m_InfosVideo->iFileSize = fileInfo->Length;
		log->Debug(String::Format("File Size:{0}.", m_InfosVideo->iFileSize));

		// 1. Ouvrir le fichier et récupérer les infos sur le format.
		if(av_open_input_file(&pFormatCtx, cFilePath , NULL, 0, NULL) != 0)
		{
			result = LoadResult::FileNotOpenned;
			log->Debug("The file could not be openned. (Wrong path or not a video.)");
			break;
		}
		Marshal::FreeHGlobal(safe_cast<IntPtr>(cFilePath));

		// 2. Obtenir les infos sur les streams contenus dans le fichier.
		if(av_find_stream_info(pFormatCtx) < 0 )
		{
			result = LoadResult::StreamInfoNotFound;
			log->Debug("The streams Infos were not Found.");
			break;
		}
		DumpStreamsInfos(pFormatCtx);

		// 3. Obtenir l'identifiant du premier stream de sous titre.
		iMetadataStreamIndex = GetFirstStreamIndex(pFormatCtx, CODEC_TYPE_SUBTITLE);

		// 4. Vérifier que ce stream est bien notre stream de meta données et pas un stream de sous-titres classique.
		// + Le parseur de meta données devra également être blindé contre les fichiers malicieusement malformés.
		if(iMetadataStreamIndex >= 0)
		{
			if( (pFormatCtx->streams[iMetadataStreamIndex]->codec->codec_id != CODEC_ID_TEXT) ||
				(strcmp((char*)pFormatCtx->streams[iMetadataStreamIndex]->language, "XML") != 0) )
			{
				log->Debug("Subtitle stream found, but not analysis meta data: will be ignored.");
				iMetadataStreamIndex = -1;
			}
		}

		// 5. Obtenir l'identifiant du premier stream vidéo.
		if( (iVideoStreamIndex = GetFirstStreamIndex(pFormatCtx, CODEC_TYPE_VIDEO)) < 0 )
		{
			result = LoadResult::VideoStreamNotFound;
			log->Debug("No Video stream found in the file. (File is audio only, or video stream is broken.)");
			break;
		}

		// 6. Obtenir un objet de paramètres du codec vidéo.
		pCodecCtx = pFormatCtx->streams[iVideoStreamIndex]->codec;
		log->Debug(String::Format("Codec							: {0}({1})", gcnew String(pCodecCtx->codec_name), (int)pCodecCtx->codec_id));
		m_InfosVideo->bIsCodecMpeg2 = (pCodecCtx->codec_id == CODEC_ID_MPEG2VIDEO);

		if( (pCodec = avcodec_find_decoder(pCodecCtx->codec_id)) == nullptr)
		{
			result = LoadResult::CodecNotFound;
			log->Debug("No suitable codec to decode the video. (Worse than an unsupported codec.)");
			break;
		}

		// 7. Ouvrir le Codec vidéo.
		if(avcodec_open(pCodecCtx, pCodec) < 0)
		{
			result = LoadResult::CodecNotOpened;
			log->Debug("Codec could not be openned. (Codec known, but not supported yet.)");
			break;
		}

		//------------------------------------------------------------------------------------------------
		// [2007-09-14]
		// On utilise plus que les timestamps pour se localiser dans la vidéo.
		// (Par pourcentage du total de timestamps)
		//------------------------------------------------------------------------------------------------
		log->Debug("Duration (frames) if available	: " + pFormatCtx->streams[iVideoStreamIndex]->nb_frames);
		log->Debug("Duration (µs)					: " + pFormatCtx->duration);
		log->Debug("Format[Stream] timeBase			: " + pFormatCtx->streams[iVideoStreamIndex]->time_base.den + ":" + pFormatCtx->streams[iVideoStreamIndex]->time_base.num);
		log->Debug("Codec timeBase					: " + pCodecCtx->time_base.den + ":" + pCodecCtx->time_base.num);

		m_InfosVideo->fAverageTimeStampsPerSeconds = (double)pFormatCtx->streams[iVideoStreamIndex]->time_base.den / (double)pFormatCtx->streams[iVideoStreamIndex]->time_base.num;
		log->Debug("Average timestamps per seconds	: " + m_InfosVideo->fAverageTimeStampsPerSeconds);
		
		// Compute total duration in TimeStamps.
		if(pFormatCtx->duration > 0)
		{
			// av_rescale ?
			m_InfosVideo->iDurationTimeStamps = (int64_t)((double)((double)pFormatCtx->duration/(double)AV_TIME_BASE)*m_InfosVideo->fAverageTimeStampsPerSeconds);
		}
		else
		{
			// todo : try SuperSeek technique. Seek @ +10 Hours, to get the last I-Frame
			m_InfosVideo->iDurationTimeStamps = 0;
		}
		log->Debug("Duration in timestamps			: " + m_InfosVideo->iDurationTimeStamps);
		log->Debug("Duration in seconds	(computed)	: " + (double)(double)m_InfosVideo->iDurationTimeStamps/(double)m_InfosVideo->fAverageTimeStampsPerSeconds);


		//----------------------------------------------------------------------------------------------------------------------
		// FPS Moyen.
		// Sur un Play, la cadence des frames ne reflètera pas forcément la vraie cadence si le fichier à un framerate variable.
		// On considère que c'est un cas rare et que la différence ne va pas trop géner.
		// 
		// Trois sources pour calculer le FPS moyen, à tester dans l'ordre :
		//
		//  - les infos de duration en frames et en µs, du conteneur et du stream. (Rarement disponibles, mais valide si oui)
		//	- le Stream->time_base	(Souvent ko, sous la forme de 90000:1, sert en fait à exprimer l'unité des timestamps)
		//  - le Codec->time_base	(Souvent ok, mais pas toujours.)
		//
		//----------------------------------------------------------------------------------------------------------------------
		m_InfosVideo->fFps = 0;
		m_InfosVideo->bFpsIsReliable = true;
		//-----------------------
		// 1.a. Par les durations
		//-----------------------
		if( (pFormatCtx->streams[iVideoStreamIndex]->nb_frames > 0) && (pFormatCtx->duration > 0))
		{	
			m_InfosVideo->fFps = ((double)pFormatCtx->streams[iVideoStreamIndex]->nb_frames * (double)AV_TIME_BASE)/(double)pFormatCtx->duration;
			log->Debug("Average Fps estimation method	: Durations.");
		}
		else
		{
		
			//-------------------------------------------------------
			// 1.b. Par le Stream->time_base, on invalide si >= 1000.
			//-------------------------------------------------------
			m_InfosVideo->fFps  = (double)pFormatCtx->streams[iVideoStreamIndex]->time_base.den / (double)pFormatCtx->streams[iVideoStreamIndex]->time_base.num;
			
			if(m_InfosVideo->fFps < 1000)
			{
				log->Debug("Average Fps estimation method	: Format[Stream] context timebase.");
			}
			else
			{
				//------------------------------------------------------
				// 1.c. Par le Codec->time_base, on invalide si >= 1000.
				//------------------------------------------------------
				m_InfosVideo->fFps = (double)pCodecCtx->time_base.den / (double)pCodecCtx->time_base.num;

				if(m_InfosVideo->fFps < 1000)
				{
					log->Debug("Average Fps estimation method	: Codec context timebase.");
				}
				else
				{
					//---------------------------------------------------------------------------
					// Le fichier ne nous donne pas assez d'infos, ou le frame rate est variable.
					// Forcer à 25 fps. 
					//---------------------------------------------------------------------------
					m_InfosVideo->fFps = 25;
					m_InfosVideo->bFpsIsReliable = false;
					log->Debug("Average Fps estimation method	: Estimation failed. Fps Forced to : " + m_InfosVideo->fFps);
				}
			}
		}

		log->Debug("Average Fps						: " + m_InfosVideo->fFps);
		
		m_InfosVideo->iFrameInterval = (int)((double)1000/m_InfosVideo->fFps);
		log->Debug("Average Frame Interval (ms)		: " + m_InfosVideo->iFrameInterval);


		// av_rescale ?
		if(pFormatCtx->start_time > 0)
			m_InfosVideo->iFirstTimeStamp = (int64_t)((double)((double)pFormatCtx->start_time/(double)AV_TIME_BASE)*m_InfosVideo->fAverageTimeStampsPerSeconds);
		else
			m_InfosVideo->iFirstTimeStamp = 0;
	
		log->Debug("Start time (µs)					: " + pFormatCtx->start_time);
		log->Debug("First timestamp					: " + m_InfosVideo->iFirstTimeStamp);

		// Précomputations.
		m_InfosVideo->iAverageTimeStampsPerFrame = (int64_t)Math::Round(m_InfosVideo->fAverageTimeStampsPerSeconds / m_InfosVideo->fFps);

		//----------------
		// Other datas.
		//----------------
		m_InfosVideo->iWidth = pCodecCtx->width; 
		m_InfosVideo->iHeight = pCodecCtx->height;
		
		log->Debug("Width (pixels)					: " + pCodecCtx->width);
		log->Debug("Height (pixels)					: " + pCodecCtx->height);

		if(pCodecCtx->sample_aspect_ratio.num != 0 && pCodecCtx->sample_aspect_ratio.num != pCodecCtx->sample_aspect_ratio.den)
		{
			// Anamorphic video, non square pixels.
			log->Debug("Display Aspect Ratio type		: Anamorphic");

			if(pCodecCtx->codec_id == CODEC_ID_MPEG2VIDEO)
			{
				// If MPEG, sample_aspect_ratio is actually the DAR...
				// Reference for weird decision tree: mpeg12.c at mpeg_decode_postinit().
				double fDisplayAspectRatio = (double)pCodecCtx->sample_aspect_ratio.num / (double)pCodecCtx->sample_aspect_ratio.den;
				m_InfosVideo->fPixelAspectRatio	= ((double)pCodecCtx->height * fDisplayAspectRatio) / (double)pCodecCtx->width;

				if(m_InfosVideo->fPixelAspectRatio < 1.0f)
				{
					m_InfosVideo->fPixelAspectRatio = fDisplayAspectRatio;
				}
			}
			else
			{
				m_InfosVideo->fPixelAspectRatio = (double)pCodecCtx->sample_aspect_ratio.num / (double)pCodecCtx->sample_aspect_ratio.den;
			}	
				
			m_InfosVideo->iSampleAspectRatioNumerator = pCodecCtx->sample_aspect_ratio.num;
			m_InfosVideo->iSampleAspectRatioDenominator = pCodecCtx->sample_aspect_ratio.den;

			log->Debug("Pixel Aspect Ratio				: " + m_InfosVideo->fPixelAspectRatio);
		}
		else
		{
			// Assume PAR=1:1.
			log->Debug("Display Aspect Ratio type		: Square Pixels");
			m_InfosVideo->fPixelAspectRatio = 1.0f;
		}

		m_InfosVideo->eAspectRatio = m_DefaultSettings->eAspectRatio;
		SetImageGeometry();

		if(m_InfosVideo->iDecodingWidth != m_InfosVideo->iWidth)
		{
			log->Debug("Width is changed to 			: " + m_InfosVideo->iDecodingWidth);
		}
		if(m_InfosVideo->iDecodingHeight != m_InfosVideo->iHeight)
		{
			log->Debug("Height is changed to			: " + m_InfosVideo->iDecodingHeight);
		}

		m_InfosVideo->bDeinterlaced = m_DefaultSettings->bDeinterlace;


		//------------------------
		// Audio.
		//------------------------
		/*
		bool bAudioLoaded = false;
		iAudioStreamIndex = GetFirstStreamIndex(pFormatCtx, CODEC_TYPE_AUDIO);
		if(iAudioStreamIndex > 0)
		{
			// 6. Obtenir un objet de paramètres du codec audio.
			pAudioCodecCtx = pFormatCtx->streams[iAudioStreamIndex]->codec;
			log->Debug(String::Format("Audio Codec : {0}({1})", gcnew String(pAudioCodecCtx->codec_name), (int)pAudioCodecCtx->codec_id));
			
			if( (pAudioCodec = avcodec_find_decoder(pAudioCodecCtx->codec_id)) == nullptr)
			{
				log->Debug("No suitable codec to decode the audio.");
			}
			else
			{
				// 7. Ouvrir le Codec audio.
				if(avcodec_open(pAudioCodecCtx, pAudioCodec) < 0)
				{
					log->Debug("Audio Codec could not be openned.");
				}
				else
				{
					log->Debug(String::Format("Audio channels : {0}", pAudioCodecCtx->channels));
					log->Debug(String::Format("Audio sample rate : {0}", pAudioCodecCtx->sample_rate));
				}
			}
			m_iAudioStream = iAudioStreamIndex;
			m_pAudioCodecCtx = pAudioCodecCtx;
		}*/

		//--------------------------------------------------------
		// Globalize Contexts (for GetNextFrame)
		//--------------------------------------------------------
		m_pFormatCtx = pFormatCtx;
		m_iVideoStream = iVideoStreamIndex;
		m_pCodecCtx	= pCodecCtx;
		m_iMetadataStream = iMetadataStreamIndex;
		m_bIsLoaded = true;				
		
		result = LoadResult::Success;
	}
	while(false);

	//--------------------------------------------------
	// CLEANUP SI KO (?)
	// (On fera un Unload, mais est-ce suffisant ?)
	//--------------------------------------------------
	log->Debug(""); 
	log->Debug("Exiting LoadMovie");
	log->Debug("---------------------------------------------------");

	return result;

}
/// <summary>
/// Unload the video and dispose unmanaged resources.
/// </summary>
void VideoFile::Unload()
{
	if(m_bIsLoaded)
	{
		ResetPrimarySelection();
		ResetInfosVideo();

		// Current AVFrame.
		if(m_pCurrentDecodedFrameBGR != nullptr)
		{
			av_free(m_pCurrentDecodedFrameBGR);
			m_pCurrentDecodedFrameBGR = nullptr;
			delete [] m_Buffer;
			m_Buffer = nullptr;
		}

		// Release images extracted to memory.
		if(m_FrameList)
		{
			for(int i = 0;i<m_FrameList->Count;i++)
			{
				delete m_FrameList[i]->BmpImage;
				delete m_FrameList[i];
			}
		
			delete m_FrameList;
		}

		// Release audio extracted to memory.
		/*if(m_AudioBuffer != nullptr)
		{
			delete [] m_AudioBuffer;
			m_AudioBuffer = nullptr;
		}*/

		// FFMpeg-close file and codec.
		if(m_pCodecCtx != nullptr)
		{
			avcodec_close(m_pCodecCtx);
		}
		
		/*if(m_pAudioCodecCtx != nullptr)
		{
			avcodec_close(m_pAudioCodecCtx);
		}*/
		if(m_pFormatCtx != nullptr)
		{
			av_close_input_file(m_pFormatCtx);
		}

		m_bIsLoaded = false;
	}
}
/// <summary>
/// Get the metadata XML string embeded in the file. null if not found.
/// </summary>
String^ VideoFile::ReadMetadata()
{
	String^ szMetadata;

	if(m_iMetadataStream >= 0)
	{
		bool done = false;
		do
		{
			AVPacket	InputPacket;
			int			iReadFrameResult;

			if( (iReadFrameResult = av_read_frame( m_pFormatCtx, &InputPacket)) >= 0)
			{
				log->Debug("GetMetadata, InputPacket, dts:" + InputPacket.dts);
				log->Debug("GetMetadata, InputPacket, stream:" + InputPacket.stream_index);

				if(InputPacket.stream_index == m_iMetadataStream)
				{
					log->Debug("Subtitle Packet found.");
					int test = InputPacket.size;
					
					szMetadata = gcnew String((char*)InputPacket.data);
					log->Debug("Meta Data: " + szMetadata);

					done = true;
				}
				else
				{
					// Not Subtitle stream : Skip packet.
				}
			}
			else
			{
				log->Debug("ERROR: av_read_frame() failed");
				break;
			}
		}
		while(!done);
		
		// Se repositionner au début du fichier
		if(av_seek_frame(m_pFormatCtx, m_iVideoStream, m_InfosVideo->iFirstTimeStamp, AVSEEK_FLAG_BACKWARD) < 0)
		{
			log->Debug("ERROR: av_seek_frame() failed");
		}
	}

	return szMetadata;
}
ReadResult VideoFile::ReadFrame(int64_t _iTimeStampToSeekTo, int _iFramesToDecode)
{
	//---------------------------------------------------------------------
	// Reads a frame and puts it into m_BmpImage for later usage by caller.
	// Input parameters : 
	// If _iTimeStampToSeekTo = -1, use _iFramesToDecode.
	// Otherwise, use _iTimeStampToSeekTo.
	// _iFramesToDecode can be negative, it means we seek to the previous frame.
	//---------------------------------------------------------------------

	ReadResult			result = ReadResult::Success;
	int					iReadFrameResult;		// result of av_read_frame. should be >= 0.
	AVFrame*			pDecodingFrameBuffer; 
	
	int					frameFinished;
	int					iSizeBuffer;
	int					iFramesDecoded	= 0;
	int					iFramesToDecode = 0;
	int64_t				iTargetTimeStamp = _iTimeStampToSeekTo;
	bool				bIsSeeking = false;

	if(!m_bIsLoaded) return ReadResult::MovieNotLoaded;

	if(m_PrimarySelection->iAnalysisMode == 1)
	{
		if(iTargetTimeStamp >= 0)
		{	
			// Retrouver la frame correspondante au TimeStamp
			m_PrimarySelection->iCurrentFrame = (int)GetFrameNumber(iTargetTimeStamp);
		}
		else
		{
			if(m_PrimarySelection->iCurrentFrame + _iFramesToDecode < 0)
			{
				// ?
				m_PrimarySelection->iCurrentFrame = 0;
			}
			else if(m_PrimarySelection->iCurrentFrame + _iFramesToDecode >= m_FrameList->Count)
			{
				// fin de zone
				m_PrimarySelection->iCurrentFrame = m_FrameList->Count -1;
			}
			else
			{
				// Cas normal.
				m_PrimarySelection->iCurrentFrame += _iFramesToDecode;
			}
		}

		m_BmpImage = m_FrameList[m_PrimarySelection->iCurrentFrame]->BmpImage;
		m_PrimarySelection->iCurrentTimeStamp = m_FrameList[m_PrimarySelection->iCurrentFrame]->iTimeStamp;

		result = ReadResult::Success;
	}
	else
	{
		//------------------------------------------------------------------
		// Decode one frame if iTargetTimeStamp is -1, seek there otherwise.
		//------------------------------------------------------------------
		if((iTargetTimeStamp >= 0) || (_iFramesToDecode < 0))
		{	
			bIsSeeking = true;
			
			if(_iFramesToDecode < 0)
			{
				// Déplacement négatif : retrouver le timestamp.
				iTargetTimeStamp = m_PrimarySelection->iCurrentTimeStamp + (_iFramesToDecode * m_InfosVideo->iAverageTimeStampsPerFrame);
				if(iTargetTimeStamp < 0) iTargetTimeStamp = 0;
			}

			//------------------------------------------------------------------------------------------
			// seek.
			// AVSEEK_FLAG_BACKWARD -> Va à la dernière I-Frame avant la position demandée.
			// necessitera de continuer à décoder tant que le PTS lu est inférieur au TimeStamp demandé.
			//------------------------------------------------------------------------------------------
			log->Debug(String::Format("Seeking to [{0}]", iTargetTimeStamp));
			av_seek_frame(m_pFormatCtx, m_iVideoStream, iTargetTimeStamp, AVSEEK_FLAG_BACKWARD);
			avcodec_flush_buffers( m_pFormatCtx->streams[m_iVideoStream]->codec);
			iFramesToDecode = 1;
		}
		else
		{
			iFramesToDecode = _iFramesToDecode;
		}
		
		// Allocate video frames, one for decoding, one to hold the picture after conversion.
		// Note: in the case of anamorphic video, the decoding frame is larger than the output one.
		pDecodingFrameBuffer = avcodec_alloc_frame();

		if(m_pCurrentDecodedFrameBGR != nullptr)
		{
			av_free(m_pCurrentDecodedFrameBGR);
		}

		m_pCurrentDecodedFrameBGR = avcodec_alloc_frame();

		if( (m_pCurrentDecodedFrameBGR != NULL) && (pDecodingFrameBuffer != NULL) )
		{
			// Figure out required size for image buffer and allocate it.
			iSizeBuffer = avpicture_get_size(PIX_FMT_BGR24, m_InfosVideo->iDecodingWidth, m_InfosVideo->iDecodingHeight);
			if(iSizeBuffer > 0)
			{
				if(m_Buffer == nullptr)
				{
					m_Buffer = new uint8_t[iSizeBuffer];
				}
				// Assign appropriate parts of buffer to image planes in pFrameBGR
				avpicture_fill((AVPicture *)m_pCurrentDecodedFrameBGR, m_Buffer , PIX_FMT_BGR24, m_InfosVideo->iDecodingWidth, m_InfosVideo->iDecodingHeight);
			
				//----------------------
				// Reading/Decoding loop
				//----------------------
				bool done = false;
				bool bFirstPass = true;

				do
				{
					// Read next packet
					AVPacket	InputPacket;
					iReadFrameResult = av_read_frame( m_pFormatCtx, &InputPacket);

					if(iReadFrameResult >= 0)
					{
						// Is this a packet from the video stream ?
						if(InputPacket.stream_index == m_iVideoStream)
						{
							// Decode video packet. This is needed even if we're not on the final frame yet.
							// I Frames data is kept internally by ffmpeg and we'll need it to build the final frame. 
							avcodec_decode_video(m_pCodecCtx, pDecodingFrameBuffer, &frameFinished, InputPacket.data, InputPacket.size);
							
							if(frameFinished)
							{
								// Update positions
								if(InputPacket.dts == AV_NOPTS_VALUE)
								{
									m_PrimarySelection->iCurrentTimeStamp = 0;
								}
								else
								{
									m_PrimarySelection->iCurrentTimeStamp = InputPacket.dts;
								}

								
								if(bIsSeeking && bFirstPass && m_PrimarySelection->iCurrentTimeStamp > iTargetTimeStamp && iTargetTimeStamp >= 0)
								{
									
									// If the current ts is already after the target, we are dealing with this kind of files
									// where the seek doesn't work as advertised. We'll seek back 1 full second behind
									// the target and then decode until we get to it.
									
									// Do this only once.
									bFirstPass = false;
									
									// place the new target one second before the original one.
									int64_t iForceSeekTimestamp = iTargetTimeStamp - (int64_t)m_InfosVideo->fAverageTimeStampsPerSeconds;
									if(iForceSeekTimestamp < 0) iForceSeekTimestamp = 0;

									// Do the seek.
									log->Debug(String::Format("First decoded frame [{0}] already after target. Force seek back 1 second to [{1}]", m_PrimarySelection->iCurrentTimeStamp, iForceSeekTimestamp));
									av_seek_frame(m_pFormatCtx, m_iVideoStream, iForceSeekTimestamp, AVSEEK_FLAG_BACKWARD);
									avcodec_flush_buffers(m_pFormatCtx->streams[m_iVideoStream]->codec);

									// Free the packet that was allocated by av_read_frame
									av_free_packet(&InputPacket);

									// Loop back.
									continue;
								}

								bFirstPass = false;
								
								iFramesDecoded++;

								//-------------------------------------------------------------------------------
								// If we're done, convert the image and store it into its final recipient.
								// - In case of a seek, if we reached the target timestamp.
								// - In case of a linear decoding, if we decoded the required number of frames.
								//-------------------------------------------------------------------------------
								if(	((iTargetTimeStamp >= 0) && (m_PrimarySelection->iCurrentTimeStamp >= iTargetTimeStamp)) ||
									((iTargetTimeStamp < 0)	&& (iFramesDecoded >= iFramesToDecode)))
								{
									done = true;

									//----------------------------------------------------------------
									// Rescale image if needed and convert formats
									// Deinterlace (must be done at original size and pix_fmt YUV420P)
									//-----------------------------------------------------------------

									bool bScaleResult = RescaleAndConvert(m_pCurrentDecodedFrameBGR, pDecodingFrameBuffer, m_InfosVideo->iDecodingWidth, m_InfosVideo->iDecodingHeight, PIX_FMT_BGR24, m_InfosVideo->bDeinterlaced);

									if(!bScaleResult && m_InfosVideo->fPixelAspectRatio != 1.0f)
									{
										// [m137]. On Vista and Seven, it may happen that the sws_scale errors out for 
										// no known reason if we change ratio (non square pixels). (Only on a specific codec though)
										// In that case, we will re-alloc the buffer and try the rescale again.

										log->Error("Discarding anamorphic info and retrying the scale.");
										ChangeAspectRatio(AspectRatio::ForceSquarePixels);
										RescaleAndConvert(m_pCurrentDecodedFrameBGR, pDecodingFrameBuffer, m_InfosVideo->iDecodingWidth, m_InfosVideo->iDecodingHeight, PIX_FMT_BGR24, m_InfosVideo->bDeinterlaced);
									}
				
									try
									{
										// Import ffmpeg buffer into a .NET bitmap.
										int iImageStride	= m_pCurrentDecodedFrameBGR->linesize[0];
										IntPtr* ptr = new IntPtr((void*)m_pCurrentDecodedFrameBGR->data[0]); 
										if(m_BmpImage) delete m_BmpImage;
										m_BmpImage = gcnew Bitmap( m_InfosVideo->iDecodingWidth, m_InfosVideo->iDecodingHeight, iImageStride, Imaging::PixelFormat::Format24bppRgb, *ptr );
									}
									catch(Exception^)
									{
										m_BmpImage = nullptr;
										result = ReadResult::ImageNotConverted;
									}

								}
								else
								{
									// frame completely decoded but target frame not reached yet.
								}
							}
							else
							{
								// Frame not complete. Keep decoding packets until it is.
							}
						
						}
						else
						{						
							// Not the video stream.
						}
						
						// Free the packet that was allocated by av_read_frame
						av_free_packet(&InputPacket);
					}
					else
					{
						
						// Reading error. We don't know if the error happened on a video frame or audio one.
						done = true;
						result = ReadResult::FrameNotRead;
					}
				}
				while(!done);
				
				// Needs to be freed here to avoid a 'yet to be investigated' crash.
				av_free(pDecodingFrameBuffer);
			}
			else
			{
				// Codec was opened succefully but we got garbage in width and height. (Some FLV files.)
				result = ReadResult::MemoryNotAllocated;
			}
			//av_free(pDecodingFrameBuffer);
		}
		else
		{
			result = ReadResult::MemoryNotAllocated;
		}

		// can't free pDecodingFrameBuffer this late ?

	}		
	return result;
}
InfosThumbnail^ VideoFile::GetThumbnail(String^ _FilePath, int _iPicWidth)
{

	InfosThumbnail^ infos = gcnew InfosThumbnail();
	infos->Thumbnails = gcnew List<Bitmap^>();
	int iMaxThumbnails = 4;
	int64_t iIntervalTimestamps = 1;
	
	Bitmap^ bmp = nullptr;
	bool bGotPicture = false;
	bool bCodecOpened = false;

	AVFormatContext* pFormatCtx = nullptr;
	AVCodecContext* pCodecCtx = nullptr;

	do
	{
		char* cFilePath = static_cast<char *>(Marshal::StringToHGlobalAnsi(_FilePath).ToPointer());

		// 1. Ouvrir le fichier et récupérer les infos sur le format.
		
		if(av_open_input_file(&pFormatCtx, cFilePath , NULL, 0, NULL) != 0)
		{
			log->Error("GetThumbnail Error : Input file not opened");
			break;
		}
		Marshal::FreeHGlobal(safe_cast<IntPtr>(cFilePath));
		
		// 2. Obtenir les infos sur les streams contenus dans le fichier.
		if(av_find_stream_info(pFormatCtx) < 0 )
		{
			log->Error("GetThumbnail Error : Stream infos not found");
			break;
		}

		// 3. Obtenir l'identifiant du premier stream vidéo.
		int iVideoStreamIndex = -1;
		if( (iVideoStreamIndex = GetFirstStreamIndex(pFormatCtx, CODEC_TYPE_VIDEO)) < 0 )
		{
			log->Error("GetThumbnail Error : First video stream not found");
			break;
		}

		// 4. Obtenir un objet de paramètres du codec vidéo.
		AVCodec* pCodec = nullptr;
		pCodecCtx = pFormatCtx->streams[iVideoStreamIndex]->codec;
		if( (pCodec = avcodec_find_decoder(pCodecCtx->codec_id)) == nullptr)
		{
			log->Error("GetThumbnail Error : Decoder not found");
			break;
		}

		// 5. Ouvrir le Codec vidéo.
		if(avcodec_open(pCodecCtx, pCodec) < 0)
		{
			log->Error("GetThumbnail Error : Decoder not opened");
			break;
		}
		bCodecOpened = true;


		// TODO:
		// Fill up a InfosThumbnail object with data.
		// (Fixes anamorphic, unsupported width, compute length, etc.)

		// 5.b Compute duration in timestamps.
		double fAverageTimeStampsPerSeconds = (double)pFormatCtx->streams[iVideoStreamIndex]->time_base.den / (double)pFormatCtx->streams[iVideoStreamIndex]->time_base.num;
		if(pFormatCtx->duration > 0)
		{
			infos->iDurationMilliseconds = pFormatCtx->duration / 1000;
			
			// Compute the interval in timestamps at which we will extract thumbs.
			int64_t iDurationTimeStamps = (int64_t)((double)((double)pFormatCtx->duration / (double)AV_TIME_BASE) * fAverageTimeStampsPerSeconds);
			iIntervalTimestamps = iDurationTimeStamps / iMaxThumbnails;
		}
		else
		{
			// No duration infos, only get one pic.
			iMaxThumbnails = 1;
		}

		// 6. Allocate video frames, one for decoding, one to hold the picture after conversion.
		AVFrame* pDecodingFrameBuffer = avcodec_alloc_frame();
		AVFrame* pDecodedFrameBGR = avcodec_alloc_frame();

		if( (pDecodedFrameBGR != NULL) && (pDecodingFrameBuffer != NULL) )
		{
			// We ask for pictures already reduced in size to lighten GDI+ burden: max at _iPicWidth px width.
			// This also takes care of image size which are not multiple of 4.
			float fWidthRatio = (float)pCodecCtx->width / _iPicWidth;
			int iDecodingWidth = _iPicWidth;
			int iDecodingHeight = (int)((float)pCodecCtx->height / fWidthRatio);

			int iSizeBuffer = avpicture_get_size(PIX_FMT_BGR24, iDecodingWidth, iDecodingHeight);
			if(iSizeBuffer < 1)
			{
				log->Error("GetThumbnail Error : Frame buffer not allocated");
				break;
			}
			uint8_t* pBuffer = new uint8_t[iSizeBuffer];

			// Assign appropriate parts of buffer to image planes in pFrameBGR
			avpicture_fill((AVPicture *)pDecodedFrameBGR, pBuffer , PIX_FMT_BGR24, iDecodingWidth, iDecodingHeight);
			
			int iTotalReadFrames = 0;
			
			//-------------------
			// Read the first frame.
			//-------------------
			bool done = false;
			do
			{
				AVPacket	InputPacket;
				
				int iReadFrameResult = av_read_frame( pFormatCtx, &InputPacket);

				if(iReadFrameResult >= 0)
				{
					// Is this a packet from the video stream ?
					if(InputPacket.stream_index == iVideoStreamIndex)
					{
						// Decode video frame
						int	frameFinished;
						avcodec_decode_video(pCodecCtx, pDecodingFrameBuffer, &frameFinished, InputPacket.data, InputPacket.size);

						if(frameFinished)
						{
							iTotalReadFrames++;
							if(iTotalReadFrames > iMaxThumbnails-1)
							{
								done=true;
							}

							SwsContext* pSWSCtx = sws_getContext(pCodecCtx->width, pCodecCtx->height, pCodecCtx->pix_fmt, iDecodingWidth, iDecodingHeight, PIX_FMT_BGR24, SWS_FAST_BILINEAR, NULL, NULL, NULL); 
							sws_scale(pSWSCtx, pDecodingFrameBuffer->data, pDecodingFrameBuffer->linesize, 0, pCodecCtx->height, pDecodedFrameBGR->data, pDecodedFrameBGR->linesize); 
							sws_freeContext(pSWSCtx);

							try
							{
								IntPtr* ptr = new IntPtr((void*)pDecodedFrameBGR->data[0]); 
								Bitmap^ tmpBitmap = gcnew Bitmap( iDecodingWidth, iDecodingHeight, pDecodedFrameBGR->linesize[0], Imaging::PixelFormat::Format24bppRgb, *ptr );
							
								//---------------------------------------------------------------------------------
								// Dupliquer complètement, 
								// Bitmap.Clone n'est pas suffisant, on continue de pointer vers les mêmes données.
								//---------------------------------------------------------------------------------
								bmp = AForge::Imaging::Image::Clone(tmpBitmap, tmpBitmap->PixelFormat);
								infos->Thumbnails->Add(bmp);
								delete tmpBitmap;
								tmpBitmap = nullptr;
								bGotPicture = true;
							}
							catch(Exception^)
							{
								log->Error("GetThumbnail Error : Bitmap creation failed");
								bmp = nullptr;
							}
							
							//-------------------------------------------
							// Seek to next image. 
							// Approximation : We don't care about first timestamp being greater than 0.	
							//-------------------------------------------
							if(iTotalReadFrames > 0 && iTotalReadFrames < iMaxThumbnails)
							{
								try
								{
									//log->Debug(String::Format("Jumping to {0} to extract thumbnail {1}.", iTotalReadFrames * iIntervalTimestamps, iTotalReadFrames+1));
									av_seek_frame(pFormatCtx, iVideoStreamIndex, (iTotalReadFrames * iIntervalTimestamps), AVSEEK_FLAG_BACKWARD);
									avcodec_flush_buffers(pFormatCtx->streams[iVideoStreamIndex]->codec);
								}
								catch(Exception^)
								{
									log->Error("GetThumbnail Error : Jumping to next extraction point failed.");
									done = true;
								}
							}
						}
						else
						{
							int iFrameNotFinished=1; // test pour debug
						}
					}
					else
					{
						// Not the first video stream.
						//Console::WriteLine("This is Stream #{0}, Loop until stream #{1}", InputPacket.stream_index, iVideoStreamIndex);
					}
					
					// Free the packet that was allocated by av_read_frame
					av_free_packet(&InputPacket);
				}
				else
				{
					// Reading error.
					done = true;
					log->Error("GetThumbnail Error : Frame reading failed");
				}
			}
			while(!done);
			
			// Clean Up
			delete []pBuffer;
			pBuffer = nullptr;
			
			av_free(pDecodingFrameBuffer);
			av_free(pDecodedFrameBGR);
		}
		else
		{
			// Not enough memory to allocate the frame.
			log->Error("GetThumbnail Error : AVFrame holders not allocated");
		}
	}
	while(false);

	if(bCodecOpened)
	{
		avcodec_close(pCodecCtx);
		//av_free(pCodecCtx);
	}

	if(pFormatCtx != nullptr)
	{
		av_close_input_file(pFormatCtx);
		//av_free(pFormatCtx);	
	}

	//Console::WriteLine("Leaving GetThumbnail, Available RAM: {0}", RamCounter->NextValue());
	//delete RamCounter;

	return infos;
}
bool VideoFile::CanExtractToMemory(int64_t _iStartTimeStamp, int64_t _iEndTimeStamp, int _maxSeconds, int _maxMemory)
{
	// Check if the current selection could switch to analysis mode, according to the current settings.
	// _maxMemory is in Mib.
	
	// To be analyzable, both conditions must be met.
	int iDurationTimeStamps = (int)(_iEndTimeStamp - _iStartTimeStamp);
	double iDurationSeconds = (double)iDurationTimeStamps / m_InfosVideo->fAverageTimeStampsPerSeconds;
	
	int iFrameMemoryBytes = avpicture_get_size(PIX_FMT_BGR24, m_InfosVideo->iDecodingWidth, m_InfosVideo->iDecodingHeight);
	double iFrameMemoryMegaBytes = (double)iFrameMemoryBytes  / 1048576;
	int iTotalFrames = (int)(iDurationTimeStamps / m_InfosVideo->iAverageTimeStampsPerFrame);
	int iDurationMemory = (int)((double)iTotalFrames * iFrameMemoryMegaBytes);
	
	bool result = false;
	if( (iDurationSeconds > 0) && (iDurationSeconds <= _maxSeconds) && (iDurationMemory <= _maxMemory))
	{
		result = true;
	}
    
	return result;
}
void VideoFile::ExtractToMemory(int64_t _iStartTimeStamp, int64_t _iEndTimeStamp, bool _bForceReload)
{
	
	int					iReadFrameResult;
	AVFrame				*pDecodingFrameBuffer; 
	AVPacket			packet;
	int					frameFinished;
	int					iSizeBuffer;
	int					iResult = 0;
	int64_t				iCurrentTimeStamp = 0;
	int					iFramesDecoded = 0;
	int64_t				iOldStart = 0;
	int64_t				iOldEnd = 0;
	bool				bCanceled = false;
	

	//---------------------------------------------------
	// Check what we need to load.
	// If reducing, reduces the selection.
	//---------------------------------------------------
	ImportStrategy strategy = PrepareSelection(_iStartTimeStamp, _iEndTimeStamp, _bForceReload);
	
	// Reinitialize the reading type in case we fail.
	m_PrimarySelection->iAnalysisMode = 0;
	
	// If not complete, we'll only decode frames we don't already have.
	if(strategy != ImportStrategy::Complete)
	{
		if(m_FrameList->Count > 0)
		{
			iOldStart = m_FrameList[0]->iTimeStamp;
			iOldEnd = m_FrameList[m_FrameList->Count - 1]->iTimeStamp;
			log->Debug(String::Format("Optimized sentinels: [{0}]->[{1}]", _iStartTimeStamp, _iEndTimeStamp));
		}
	}
	
	if(strategy != ImportStrategy::Reduction)
	{
		int  iEstimatedNumberOfFrames = EstimateNumberOfFrames(_iStartTimeStamp, _iEndTimeStamp);

		//-----------------------------------------
		// Seek au début de la selection (ou avant)
		// TODO : et si ret = -1 ?
		//-----------------------------------------
		int ret = av_seek_frame(m_pFormatCtx, m_iVideoStream, _iStartTimeStamp, AVSEEK_FLAG_BACKWARD);
		avcodec_flush_buffers(m_pFormatCtx->streams[m_iVideoStream]->codec);

		//-----------------------------------------------------------------------------------
		// Allocate video frames, one for decoding, one to hold the picture after conversion.
		//-----------------------------------------------------------------------------------
		pDecodingFrameBuffer = avcodec_alloc_frame();
		if(m_pCurrentDecodedFrameBGR != nullptr)
		{
			av_free(m_pCurrentDecodedFrameBGR);
		}
		m_pCurrentDecodedFrameBGR = avcodec_alloc_frame();

		
		/*
		// Allocate the all input audio buffer.
		if(m_AudioBuffer != nullptr)
		{
			delete [] m_AudioBuffer;
			m_AudioBuffer = nullptr;
		}
		
		// Find selection duration in seconds and allocate for that x AVCODEC_MAX_AUDIO_FRAME_SIZE.
		m_AudioBuffer = new uint8_t[10 * AVCODEC_MAX_AUDIO_FRAME_SIZE];
		m_AudioBufferUsedSize = 0;
		*/
		
		if( (m_pCurrentDecodedFrameBGR != NULL) && (pDecodingFrameBuffer != NULL) )
		{
			// Final container (customized image size)
			iSizeBuffer = avpicture_get_size(PIX_FMT_BGR24, m_InfosVideo->iDecodingWidth, m_InfosVideo->iDecodingHeight);
			if(iSizeBuffer > 0)
			{
				if(m_Buffer == nullptr)
					m_Buffer = new uint8_t[iSizeBuffer];

				// Assign appropriate parts of buffer to image planes in pFrameBGR
				avpicture_fill((AVPicture *)m_pCurrentDecodedFrameBGR, m_Buffer , PIX_FMT_BGR24, m_InfosVideo->iDecodingWidth, m_InfosVideo->iDecodingHeight);

				
				bool done = false;
				bool bFirstPass = true;

				//-----------------
				// Read the frames.
				//-----------------
				do
				{
					iReadFrameResult = av_read_frame( m_pFormatCtx, &packet);

					if(iReadFrameResult >= 0)
					{
						// Is this a packet from the video stream?
						if(packet.stream_index == m_iVideoStream)
						{
							// Decode video frame
							avcodec_decode_video(m_pCodecCtx, pDecodingFrameBuffer, &frameFinished, packet.data, packet.size);

							// L'image est elle complète ? (En cas de B-frame ?)
							if(frameFinished)
							{
								if(packet.dts == AV_NOPTS_VALUE)
								{
									iCurrentTimeStamp = 0;
								}
								else
								{
									iCurrentTimeStamp = packet.dts;
								}

								if(bFirstPass && iCurrentTimeStamp > _iStartTimeStamp && _iStartTimeStamp >= 0)
								{
									// If the current ts is already after the target, we are dealing with this kind of files
									// where the seek doesn't work as advertised. We'll seek back 1 full second behind
									// the target and then decode until we get to it.
									
									// Do this only once.
									bFirstPass = false;

									// Place the new target one second before the original one.
									int64_t iForceSeekTimestamp = _iStartTimeStamp - (int64_t)m_InfosVideo->fAverageTimeStampsPerSeconds;
									if(iForceSeekTimestamp < 0) iForceSeekTimestamp = 0;

									// Do the seek
									log->Error(String::Format("First decoded frame [{0}] already after start stamp. Force seek back 1 second to [{1}]", iCurrentTimeStamp, iForceSeekTimestamp));
									av_seek_frame(m_pFormatCtx, m_iVideoStream, iForceSeekTimestamp, AVSEEK_FLAG_BACKWARD);
									avcodec_flush_buffers(m_pFormatCtx->streams[m_iVideoStream]->codec);

									// Free the packet that was allocated by av_read_frame
									av_free_packet(&packet);

									// Loop back.
									continue;
								}

								bFirstPass = false;

								// Attention, comme on a fait un seek, il est possible qu'on soit en train de décoder des images
								// situées AVANT le début de la selection. Tant que c'est le cas, on décode dans le vide.
								if( iCurrentTimeStamp >= _iStartTimeStamp /*&& !bSeekAgain*/)
								{
									iFramesDecoded++;

									if((_iEndTimeStamp > 0) && (iCurrentTimeStamp >= _iEndTimeStamp))
									{
										done = true;
									}
									
									RescaleAndConvert(m_pCurrentDecodedFrameBGR, pDecodingFrameBuffer, m_InfosVideo->iDecodingWidth, m_InfosVideo->iDecodingHeight, PIX_FMT_BGR24, m_InfosVideo->bDeinterlaced); 
									
									try
									{
										//------------------------------------------------------------------------------------
										// Accepte uniquement un Stride multiple de 4.
										// Tous les fichiers aux formats non standard sont refusés par le constructeur Bitmap.
										//------------------------------------------------------------------------------------
										IntPtr* ptr = new IntPtr((void*)m_pCurrentDecodedFrameBGR->data[0]); 
										int iImageStride	= m_pCurrentDecodedFrameBGR->linesize[0];
										Bitmap^ m_BmpImage = gcnew Bitmap( m_InfosVideo->iDecodingWidth, m_InfosVideo->iDecodingHeight, iImageStride, Imaging::PixelFormat::Format24bppRgb, *ptr );
										
										//-------------------------------------------------------------------------------------------------------
										// Dupliquer complètement, 
										// sinon toutes les images vont finir par utiliser le même pointeur : m_pCurrentDecodedFrameBGR->data[0].
										// Bitmap.Clone n'est pas suffisant, on continue de pointer vers les mêmes données.
										//------------------------------------------------------------------------------------------------------- 
										DecompressedFrame^ DecFrame = gcnew DecompressedFrame();
										DecFrame->BmpImage = AForge::Imaging::Image::Clone(m_BmpImage, m_BmpImage->PixelFormat); 
										DecFrame->iTimeStamp = iCurrentTimeStamp;

										//---------------------------------------------------------------------------
										// En modes d'agrandissement, 
										// faire attention au chevauchement de la selection demandée avec l'existant.
										//---------------------------------------------------------------------------
										if((strategy == ImportStrategy::InsertionBefore) && (iCurrentTimeStamp < iOldStart))
										{
											log->Debug(String::Format("Inserting frame before the original selection - [{0}]", iCurrentTimeStamp));	
											m_FrameList->Insert(iFramesDecoded - 1, DecFrame);
										}
										else if((strategy == ImportStrategy::InsertionAfter) && (iCurrentTimeStamp > iOldEnd))
										{
											log->Debug(String::Format("Inserting frame after the original selection - [{0}]", iCurrentTimeStamp));
											m_FrameList->Add(DecFrame);
										}
										else if(strategy == ImportStrategy::Complete)
										{
											//log->Debug(String::Format("Appending frame to selection - [{0}]", iCurrentTimeStamp));
											m_FrameList->Add(DecFrame);
										}
										else
										{
											// We already have this one. Do nothing.
											log->Error(String::Format("Frame not imported. Already in selection - [{0}]", iCurrentTimeStamp ));
										}
										
										delete m_BmpImage;

										// Avoid crashing if there's a refresh of the main screen.
										m_BmpImage = DecFrame->BmpImage;

										// Report Progress
										if(m_bgWorker != nullptr)
										{
											// Check for cancellation.
											if(m_bgWorker->CancellationPending)
											{
												bCanceled = true;
												done = true;
											}
											else
											{
												m_bgWorker->ReportProgress(iFramesDecoded, iEstimatedNumberOfFrames);
											}
										}
									}
									catch(Exception^)
									{
										// TODO sortir en erreur desuite.
										done = true;
										log->Error("Conversion error during selection import.");
									}
								}
								else
								{
									log->Debug(String::Format("Decoded frame is before the new start sentinel - [{0}]", iCurrentTimeStamp));
								}
							}
							else
							{
								int iFrameNotFinished=1; // test pour debug
							}
						}
						else if(packet.stream_index == m_iAudioStream)
						{
							
							// Audio experimental code.
							// done: extract audio frames to a single buffer for the whole selection.
							// todo: convert to usable format.
							// todo: play sound.

							/*

							//-----------------------------------
							// Decode audio frame(s)
							// The packet can contain multiples audio frames (samples).
							// We buffer everything into pAudioBuffer and then copy this 
							// to our own buffer for the whole selection.
							//-----------------------------------
							
							// Next section inspired by ffmpeg-sharp code.

							// Prepare buffer. It will contain up to 1 second of samples.
							uint8_t* pAudioBuffer = nullptr;
							int iAudioBufferSize = FFMAX(packet.size * sizeof(*pAudioBuffer), AVCODEC_MAX_AUDIO_FRAME_SIZE);
							pAudioBuffer = (uint8_t*)av_malloc(iAudioBufferSize);
							log->Debug(String::Format("audio samples, allocated: {0}.", iAudioBufferSize));

							int iPacketSize = packet.size;
							uint8_t* pPacketData = (uint8_t*)packet.data;
							int iTotalOutput = 0;

							// May loop multiple times if more than one frame in the packet.
							do
							{
								//-------------------------------------------------------------------------------------
								// pAudioBuffer : the entire output buffer (will contain data from all samples in the packet).
								// iAudioBufferSize : size of the entire output buffer.
								// pcmWritePtr : pointer to the current position in the output buffer.
								// iTotalOutput : number of bytes read until now, for all samples in the packet.
								//
								// pPacketData : current position in the packet.
								// iPacketSize : remaining number of bytes to read from the packet.
								//
								// iOutputBufferUsedSize : number of bytes the current sample represents once decoded.
								// iUsedInputBytes : number of bytes read from the packet to decode the current sample.
								//-------------------------------------------------------------------------------------
							
								int16_t* pcmWritePtr = (int16_t*)(pAudioBuffer + iTotalOutput);
								int iOutputBufferUsedSize = iAudioBufferSize - iTotalOutput; 
								
								log->Debug(String::Format("Decoding part of an audio packet. iOutputBufferUsedSize:{0}, iTotalOutput:{1}", iOutputBufferUsedSize, iTotalOutput));

								// Decode.
								int iUsedInputBytes = avcodec_decode_audio2(m_pAudioCodecCtx, pcmWritePtr, &iOutputBufferUsedSize, pPacketData, iPacketSize);

								log->Debug(String::Format("Decoding part of an audio packet. iUsedInputBytes:{0}", iUsedInputBytes));

								if (iUsedInputBytes < 0)
								{
									log->Debug("Audio decoding error. Ignoring packet");
									break;
								}

								if (iOutputBufferUsedSize > 0)
								{
									iTotalOutput += iOutputBufferUsedSize;
								}

								pPacketData += iUsedInputBytes;
								iPacketSize -= iUsedInputBytes;
							}
							while (iPacketSize > 0);

							log->Debug(String::Format("iTotalOutput : {0}.", iTotalOutput));

							// Convert packet to usable format.
							//todo

							// Commit these samples to main buffer.
							log->Debug(String::Format("Commiting to main buffer m_AudioBufferUsedSize : {0}.", m_AudioBufferUsedSize));
							for(int i = 0;i<iTotalOutput;i++)
							{
								m_AudioBuffer[m_AudioBufferUsedSize + i] = pAudioBuffer[i];
							}
							m_AudioBufferUsedSize += iTotalOutput;

							// Cleaning
							av_free(pAudioBuffer);

							*/
						}
						
						// Free the packet that was allocated by av_read_frame
						av_free_packet(&packet);
					}
					else
					{
						// Terminaison par fin du parcours de l'ensemble de la vidéo, ou par erreur...
						done = true;
					}
				}
				while(!done);
			
				av_free(pDecodingFrameBuffer);
			}
			else
			{
				// Codec was opened succefully but we got garbage in width and height. (Some FLV files.)
				iResult = 2;	// SHOW_NEXT_FRAME_ALLOC_ERROR
			}
		}
		else
		{
			iResult = 2;	// SHOW_NEXT_FRAME_ALLOC_ERROR
		}


	}
	// If reduction, frames were deleted at PrepareSelection time.
	

	//----------------------------
	// Fin de l'import.
	//----------------------------
	if(m_FrameList->Count > 0)
	{
		if(bCanceled)
		{
			log->Debug("Extraction to memory was cancelled, discarding PrimarySelection.");
			m_PrimarySelection->iAnalysisMode = 0;
			m_PrimarySelection->iDurationFrame = 0;
			m_PrimarySelection->iCurrentFrame = -1;
			DeleteFrameList();

			// We don't destroy m_BmpImage here, because it might already have been filled back in another thread.
			// (When we cancel the bgWorker the program control was immediately returned.)
		}
		else
		{
			m_PrimarySelection->iAnalysisMode = 1;
			m_PrimarySelection->iDurationFrame = m_FrameList->Count;
			if(m_PrimarySelection->iCurrentFrame > m_PrimarySelection->iDurationFrame-1)
			{
				m_PrimarySelection->iCurrentFrame = m_PrimarySelection->iDurationFrame - 1;
			}
			else if(m_PrimarySelection->iCurrentFrame < 0)
			{
				m_PrimarySelection->iCurrentFrame = 0;
			}

			// Image en cours
			m_BmpImage = m_FrameList[m_PrimarySelection->iCurrentFrame]->BmpImage;


			// Test save audio in raw file.
			/*IntPtr ptr((void*)m_AudioBuffer);
			array<Byte>^ bytes = gcnew array<Byte>(m_AudioBufferUsedSize);
			Marshal::Copy(ptr, bytes, 0, m_AudioBufferUsedSize);
			File::WriteAllBytes("testrawaudio.raw", bytes);*/
		}
	}
	else
	{
		log->Error("Extraction to memory failed, discarding PrimarySelection.");
		m_PrimarySelection->iAnalysisMode = 0;
		m_PrimarySelection->iDurationFrame = 0;
		m_PrimarySelection->iCurrentFrame = -1;
		DeleteFrameList();
		// /!\ m_pCurrentDecodedFrameBGR has been invalidated.
		m_BmpImage = nullptr;
	}

}

SaveResult VideoFile::Save( String^ _FilePath, int _iFramesInterval, int64_t _iSelStart, int64_t _iSelEnd, String^ _Metadata, bool _bFlushDrawings, bool _bKeyframesOnly, bool _bPausedVideo, DelegateGetOutputBitmap^ _delegateGetOutputBitmap)
{

	//---------------------------------------------
	// /!\ Refactoring in progress.
	//---------------------------------------------

	// Refactoring notes:
	// Later on, remove selection sentinels from parameters and directly use PrimarySelection or InfosVideo.
	// Same for Frames interval ?
	

	//------------------------------------------------------------------------------------
	// Input parameters depending on type of save:
	// Classic save : 
	//		_iFramesInterval = used for all frames 
	//		_bFlushDrawings = true or false
	//		_bKeyframesOnly = false.
	// Diaporama : 
	//		_iFramesInterval = used for keyframes. Other frames aren't saved anyway.
	//		_bFlushDrawings = true.
	//		_bKeyframesOnly = true.
	// Paused Video : 
	//		_iFramesInterval = used for keyframes only, other frames at original interval. 
	//		_bFlushDrawings = true.
	//		_bKeyframesOnly = false.
	//	In this last case, _iFramesInterval must be a multiple of original interval.
	//------------------------------------------------------------------------------------


	log->Debug(String::Format("Saving selection [{0}]->[{1}] to: {2}", _iSelStart, _iSelEnd, Path::GetFileName(_FilePath)));

	SaveResult result = SaveResult::Success;
	
	do
	{
		if(!m_bIsLoaded) 
		{
			result = SaveResult::MovieNotLoaded;
			break;
		}
	
		if(_delegateGetOutputBitmap == nullptr)
		{
			result = SaveResult::UnknownError;
			break;
		}		
		
		VideoFileWriter^ writer = gcnew VideoFileWriter();

		// 1. Get required parameters for opening the saving context.
		bool bHasMetadata = (_Metadata->Length > 0);
		int iFramesInterval = 40;
		int iDuplicateFactor = 1;

		if(_iFramesInterval > 0) 
		{
			if(_bPausedVideo)
			{
				// bPausedVideo is a mode where the video runs at the same speed as the original
				// except for the key images which are paused for _iFramesInterval.
				// In this case, _iFramesInterval should be a multiple of the original frame interval.
						
				iDuplicateFactor = _iFramesInterval / m_InfosVideo->iFrameInterval;
				iFramesInterval = m_InfosVideo->iFrameInterval;
			}
			else
			{
				// In normal mode, the frame interval can not go down indefinitely.
				// We can't save at less than 8 fps, so we duplicate frames when necessary.
				
				iDuplicateFactor = (int)Math::Ceiling((double)_iFramesInterval / 125.0);
				iFramesInterval = _iFramesInterval  / iDuplicateFactor;	
				log->Debug(String::Format("iFramesInterval:{0}, iDuplicateFactor:{1}", iFramesInterval, iDuplicateFactor));
			}			
		}

		// 2. Open the saving context.
		result = writer->OpenSavingContext(	_FilePath, 
											m_InfosVideo,
											iFramesInterval,
											bHasMetadata);
		
		if(result != SaveResult::Success)
		{
			log->Error("Saving context not opened.");
			break;
		}

		// 3. Write metadata if needed.
		if(bHasMetadata)
		{
			if((result = writer->SaveMetadata(_Metadata)) != SaveResult::Success)
			{
				log->Error("Metadata not saved.");
				break;
			}
		}

		// 4. Loop through input frames and save them.
		// We use two different loop depending if frames are already available or not.
		if(m_PrimarySelection->iAnalysisMode == 1)
		{
			log->Debug("Analysis mode: looping through images already extracted in memory.");

			for(int i=0;i<m_FrameList->Count;i++)
			{
				Bitmap^ InputBitmap = AForge::Imaging::Image::Clone(m_FrameList[i]->BmpImage, m_FrameList[i]->BmpImage->PixelFormat);

				// following block is duplicated in both analysis mode loop and normal mode loop.

				// Commit drawings on image if needed.
				// The function returns false if we asked for kf only and we are not on a kf.
				bool bIsKeyImage = _delegateGetOutputBitmap(Graphics::FromImage(InputBitmap), m_FrameList[i]->iTimeStamp, _bFlushDrawings, _bKeyframesOnly);
				
				if(bIsKeyImage || !_bKeyframesOnly)
				{
					if(_bPausedVideo && !bIsKeyImage)
					{
						// Normal images in paused video mode are played at normal speed.
						// In paused video mode duplicate factor only applies to the key images.
						result = writer->SaveFrame(InputBitmap);
						if(result != SaveResult::Success)
						{
							log->Error("Frame not saved.");
						}
					}
					else
					{
						for(int iDuplicate=0;iDuplicate<iDuplicateFactor;iDuplicate++)
						{
							result = writer->SaveFrame(InputBitmap);
							if(result != SaveResult::Success)
							{
								log->Error("Frame not saved.");
							}
						}
					}
				}
				
				delete InputBitmap;

				// Report progress.
				if(m_bgWorker != nullptr)
				{
					m_bgWorker->ReportProgress(i+1, m_FrameList->Count);
				}

				if(result != SaveResult::Success)
					break;
			}
		}
		else
		{
			ReadResult res = ReadResult::Success;
			
			log->Debug("Normal mode: looping to read images from the file.");
			
			bool bFirstFrame = true;
			bool done = false;
			do
			{
				if(bFirstFrame)
				{
					// Reading the first frame individually as we need to ensure we are reading the right one.
					// (some codecs go too far on the initial seek.)
					res = ReadFrame(_iSelStart, 1);
					log->Debug(String::Format("After first frame ts: {0}", m_PrimarySelection->iCurrentTimeStamp));
					bFirstFrame = false;
				}
				else
				{
					res = ReadFrame(-1, 1);
				}

				if(res == ReadResult::Success)
				{
					// Get a bitmap version.
					Bitmap^ InputBitmap = AForge::Imaging::Image::Clone(m_BmpImage, m_BmpImage->PixelFormat);

					// Commit drawings on image if needed.
					// The function returns false if we asked for kf only and we are not on a kf.
					bool bIsKeyImage = _delegateGetOutputBitmap(Graphics::FromImage(InputBitmap), m_PrimarySelection->iCurrentTimeStamp, _bFlushDrawings, _bKeyframesOnly);
					if(!_bKeyframesOnly || bIsKeyImage)
					{
						if(_bPausedVideo && !bIsKeyImage)
						{
							// Normal images in paused video mode are played at normal speed.
							// In paused video mode duplicate factor only applies to the key images.
							result = writer->SaveFrame(InputBitmap);
							if(result != SaveResult::Success)
							{
								log->Error("Frame not saved.");
							}
						}
						else
						{
							for(int iDuplicate=0;iDuplicate<iDuplicateFactor;iDuplicate++)
							{
								result = writer->SaveFrame(InputBitmap);
								if(result != SaveResult::Success)
								{
									log->Error("Frame not saved.");
									done = true;
								}
							}
						}
					}

					delete InputBitmap;

					// Report progress.
					if(m_bgWorker != nullptr)
					{
						m_bgWorker->ReportProgress((int)(m_PrimarySelection->iCurrentTimeStamp - _iSelStart), (int)(_iSelEnd - _iSelStart));
					}
					
					if(m_PrimarySelection->iCurrentTimeStamp >= _iSelEnd)
					{
						done = true;
					}
				}
				else
				{
					// This can be normal as when the file is over we get an FrameNotRead error.
					if(res != ReadResult::FrameNotRead) 
					{
						result = SaveResult::ReadingError;
					}
					
					done = true;
				}
			}
			while(!done);
		}
		
		// Close the saving context.
		writer->CloseSavingContext(true);
	
	}
	while(false);

	return result;
}
int64_t VideoFile::GetTimeStamp(int64_t _iPosition)
{
	// TimeStamp from Frame Number
	int iFrameNumber = (int)_iPosition;

	if(m_PrimarySelection->iAnalysisMode == 1)
	{
		// It is assumed here that m_FrameList is not nullptr and m_FrameList->Count > 0. 
		if(iFrameNumber > m_FrameList->Count - 1)
		{
			iFrameNumber = m_FrameList->Count - 1;
		}
		else if(iFrameNumber < 0)
		{
			iFrameNumber = 0;
		}

		return m_FrameList[iFrameNumber]->iTimeStamp;
	}
	else
	{
		return _iPosition;
	}
}
int64_t VideoFile::GetFrameNumber(int64_t _iPosition)
{
	// Frame Number from TimeStamp.
	int iFrame = 0;
	if(m_FrameList != nullptr)
	{
		if(m_FrameList->Count > 0)
		{
			int64_t iCurrentTimeStamp = m_FrameList[0]->iTimeStamp;		
			while((iCurrentTimeStamp < _iPosition) && (iFrame < m_FrameList->Count-1))
			{
				iFrame++;
				iCurrentTimeStamp = m_FrameList[iFrame]->iTimeStamp;
			}
		}
	}

	return iFrame;
}

void VideoFile::ChangeAspectRatio(AspectRatio _aspectRatio)
{
	// User changed aspect ratio.
	m_InfosVideo->eAspectRatio = _aspectRatio;
	SetImageGeometry();
	log->Debug("Image geometry modified. New height is : " + m_InfosVideo->iDecodingHeight);
	
	
	// This is a case where we must change the image size dynamically.
	// Frames extracted to memory should be discarded.
	// Drawings will be off, but that's another story.

	
}

// --------------------------------------- Private Methods

void VideoFile::ResetPrimarySelection()
{
	m_PrimarySelection->iAnalysisMode		= 0;
	m_PrimarySelection->bFiltered			= false;
	m_PrimarySelection->iCurrentFrame		= 0;
	m_PrimarySelection->iCurrentTimeStamp	= 0;
	m_PrimarySelection->iDurationFrame		= 0;
}
void VideoFile::ResetInfosVideo()
{
	// Read only
	m_InfosVideo->iFileSize = 0;
	m_InfosVideo->iWidth = 320;
	m_InfosVideo->iHeight = 240;
	m_InfosVideo->fPixelAspectRatio = 1.0f;
	m_InfosVideo->fFps = 1.0f;
	m_InfosVideo->bFpsIsReliable = false;
	m_InfosVideo->iFrameInterval = 40;
	m_InfosVideo->iDurationTimeStamps = 1;
	m_InfosVideo->iFirstTimeStamp = 0;
	m_InfosVideo->fAverageTimeStampsPerSeconds = 1.0f;
	
	// Read / Write
	m_InfosVideo->iDecodingWidth = 320;
	m_InfosVideo->iDecodingHeight = 240;
	m_InfosVideo->fDecodingStretchFactor = 1.0f;
	m_InfosVideo->iDecodingFlag = SWS_FAST_BILINEAR;
	m_InfosVideo->bDeinterlaced = false;
} 






int VideoFile::GetFirstStreamIndex(AVFormatContext* _pFormatCtx, int _iCodecType)
{
	//-----------------------------------------------
	// Look for the first stream of type _iCodecType.
	// return its index if found, -1 otherwise.
	//-----------------------------------------------

	unsigned int	iCurrentStreamIndex		= 0;
	unsigned int	iBestStreamIndex		= -1;
	int64_t			iBestFrames				= -1;

	// We loop around all streams and keep the one with most frames.
	while( iCurrentStreamIndex < _pFormatCtx->nb_streams)
	{
		if(_pFormatCtx->streams[iCurrentStreamIndex]->codec->codec_type == _iCodecType)
		{
			int64_t frames = _pFormatCtx->streams[iCurrentStreamIndex]->nb_frames;
			if(frames > iBestFrames)
			{
				iBestFrames = frames;
				iBestStreamIndex = iCurrentStreamIndex;
			}
		}
		iCurrentStreamIndex++;
	}

	return (int)iBestStreamIndex;
}

void VideoFile::DumpStreamsInfos(AVFormatContext* _pFormatCtx)
{
	log->Debug("Total Streams					: " + _pFormatCtx->nb_streams);

	for(int i = 0;i<(int)_pFormatCtx->nb_streams;i++)
	{
		log->Debug("Stream #" + i);
		switch((int)_pFormatCtx->streams[i]->codec->codec_type)
		{
		case CODEC_TYPE_UNKNOWN:
			log->Debug("	Type						: CODEC_TYPE_UNKNOWN");
			break;
		case CODEC_TYPE_VIDEO:
			log->Debug("	Type						: CODEC_TYPE_VIDEO");
			break;
		case CODEC_TYPE_AUDIO:
			log->Debug("	Type						: CODEC_TYPE_AUDIO");
			break;
		case CODEC_TYPE_DATA:
			log->Debug("	Type						: CODEC_TYPE_DATA");
			break;
		case CODEC_TYPE_SUBTITLE:
			log->Debug("	Type						: CODEC_TYPE_SUBTITLE");
			break;
		//case CODEC_TYPE_ATTACHMENT:
		//	log->Debug("	Type						: CODEC_TYPE_ATTACHMENT");
		//	break;
		default:
			log->Debug("	Type						: CODEC_TYPE_UNKNOWN");
			break;
		}
		log->Debug("	Language					: " + gcnew String(_pFormatCtx->streams[i]->language));
		log->Debug("	NbFrames					: " + _pFormatCtx->streams[i]->nb_frames);
	}
}





ImportStrategy VideoFile::PrepareSelection(int64_t% _iStartTimeStamp, int64_t% _iEndTimeStamp, bool _bForceReload)
{
	//--------------------------------------------------------------
	// détermine si la selection à réellement besoin d'être chargée.
	// Modifie simplement la selection en cas de réduction.
	// Spécifie où et quelles frames doivent être chargées sinon.
	//--------------------------------------------------------------

	ImportStrategy result;

	if(m_PrimarySelection->iAnalysisMode == 0 || _bForceReload)
	{
		//----------------------------------------------------------------------------		
		// On était pas en mode Analyse ou forcé : Chargement complet.
		// (Garder la liste même quand on sort du mode analyse, pour réutilisation ? )
		//----------------------------------------------------------------------------
		DeleteFrameList();

		log->Debug(String::Format("Preparing Selection for import : [{0}]->[{1}].", _iStartTimeStamp, _iEndTimeStamp));
		m_FrameList = gcnew List<DecompressedFrame^>();

		result = ImportStrategy::Complete; 
	}
	else
	{
		// Traitement différent selon les frames déjà chargées...
		
		if(m_FrameList == nullptr)
		{
			// Ne devrait pas passer par là.
			m_FrameList = gcnew List<DecompressedFrame^>();
			result = ImportStrategy::Complete; 
		}
		else
		{
			int64_t iOldStart = m_FrameList[0]->iTimeStamp;
			int64_t iOldEnd = m_FrameList[m_FrameList->Count - 1]->iTimeStamp;

			log->Debug(String::Format("Preparing Selection for import. Current selection: [{0}]->[{1}], new selection: [{2}]->[{3}].", iOldStart, iOldEnd, _iStartTimeStamp, _iEndTimeStamp));

			// Since some videos are causing problems in timestamps reported, it is possible that we end up with double updates.
			// e.g. reduction at left AND expansion at right, expansion on both sides, etc.
			// We'll deal with reduction first, then expansions.

			if(_iEndTimeStamp < iOldEnd)
			{
				log->Debug("Frames needs to be deleted at the end of the existing selection.");					
				int iNewLastIndex = (int)GetFrameNumber(_iEndTimeStamp);
				
				for(int i=iNewLastIndex+1;i<m_FrameList->Count;i++)
				{
					delete m_FrameList[i]->BmpImage;
				}
				m_FrameList->RemoveRange(iNewLastIndex+1, (m_FrameList->Count-1) - iNewLastIndex);

				// Reduced until further notice.
				result = ImportStrategy::Reduction;
			}

			if(_iStartTimeStamp > iOldStart)
			{
				log->Debug("Frames needs to be deleted at the begining of the existing selection.");
				int iNewFirstIndex = (int)GetFrameNumber(_iStartTimeStamp);
				
				for(int i=0;i<iNewFirstIndex;i++)
				{
					delete m_FrameList[i]->BmpImage;
				}

				m_FrameList->RemoveRange(0, iNewFirstIndex);
				
				// Reduced until further notice.
				result = ImportStrategy::Reduction;
			}


			// We expand the selection if the new sentinel is at least one frame out the current selection.
			// Expanding on both sides is not supported yet.

			if(_iEndTimeStamp >= iOldEnd + m_InfosVideo->iAverageTimeStampsPerFrame)
			{
				log->Debug("Frames needs to be added at the end of the existing selection.");
				_iStartTimeStamp = iOldEnd;
				result = ImportStrategy::InsertionAfter;
			}
			else if(_iStartTimeStamp <= iOldStart - m_InfosVideo->iAverageTimeStampsPerFrame)
			{
				log->Debug("Frames needs to be added at the begining of the existing selection.");
				_iEndTimeStamp = iOldStart;
				result = ImportStrategy::InsertionBefore;
			}
		}
	}

	return result;
}
void VideoFile::DeleteFrameList()
{
	if(m_FrameList != nullptr)
	{
		for(int i = 0;i<m_FrameList->Count;i++) 
		{ 
			delete m_FrameList[i]->BmpImage;
			delete m_FrameList[i]; 
		}			
		delete m_FrameList;
	}
}

int VideoFile::EstimateNumberOfFrames( int64_t _iStartTimeStamp, int64_t _iEndTimeStamp) 
{
	//-------------------------------------------------------------
	// Calcul du nombre d'images à charger (pour le ReportProgress)
	//-------------------------------------------------------------
	int iEstimatedNumberOfFrames = 0;
	int64_t iSelectionDurationTimeStamps;
	if(_iEndTimeStamp == -1) 
	{ 
		iSelectionDurationTimeStamps = m_InfosVideo->iDurationTimeStamps - _iStartTimeStamp; 
	}
	else 
	{ 
		iSelectionDurationTimeStamps = _iEndTimeStamp - _iStartTimeStamp; 
	}

	iEstimatedNumberOfFrames = (int)(iSelectionDurationTimeStamps / m_InfosVideo->iAverageTimeStampsPerFrame);

	return iEstimatedNumberOfFrames;
}
bool VideoFile::RescaleAndConvert(AVFrame* _pOutputFrame, AVFrame* _pInputFrame, int _OutputWidth, int _OutputHeight, int _OutputFmt, bool _bDeinterlace)
{
	//------------------------------------------------------------------------
	// Function used by GetNextFrame, ImportAnalysis and SaveMovie.
	// Take the frame we just decoded and turn it to the right size/deint/fmt.
	// todo: sws_getContext could be done only once.
	//------------------------------------------------------------------------
	bool bSuccess = true;
	SwsContext* pSWSCtx = sws_getContext(m_pCodecCtx->width, m_pCodecCtx->height, m_pCodecCtx->pix_fmt, _OutputWidth, _OutputHeight, (PixelFormat)_OutputFmt, m_InfosVideo->iDecodingFlag, NULL, NULL, NULL); 
		
	uint8_t** ppOutputData = nullptr;
	int* piStride = nullptr;
	uint8_t* pDeinterlaceBuffer = nullptr;

	if(_bDeinterlace)
	{
		AVPicture*	pDeinterlacingFrame;
		AVPicture	tmpPicture;

		// Deinterlacing happens before resizing.
		int iSizeDeinterlaced = avpicture_get_size(m_pCodecCtx->pix_fmt, m_pCodecCtx->width, m_pCodecCtx->height);
		
		pDeinterlaceBuffer = new uint8_t[iSizeDeinterlaced];
		pDeinterlacingFrame = &tmpPicture;
		avpicture_fill(pDeinterlacingFrame, pDeinterlaceBuffer, m_pCodecCtx->pix_fmt, m_pCodecCtx->width, m_pCodecCtx->height);

		int resDeint = avpicture_deinterlace(pDeinterlacingFrame, (AVPicture*)_pInputFrame, m_pCodecCtx->pix_fmt, m_pCodecCtx->width, m_pCodecCtx->height);

		if(resDeint < 0)
		{
			// Deinterlacing failed, use original image.
			log->Debug("Deinterlacing failed, use original image.");
			//sws_scale(pSWSCtx, _pInputFrame->data, _pInputFrame->linesize, 0, m_pCodecCtx->height, _pOutputFrame->data, _pOutputFrame->linesize); 
			ppOutputData = _pInputFrame->data;
			piStride = _pInputFrame->linesize;
		}
		else
		{
			// Use deinterlaced image.
			//sws_scale(pSWSCtx, pDeinterlacingFrame->data, pDeinterlacingFrame->linesize, 0, m_pCodecCtx->height, _pOutputFrame->data, _pOutputFrame->linesize); 
			ppOutputData = pDeinterlacingFrame->data;
			piStride = pDeinterlacingFrame->linesize;
		}
	}
	else
	{
		//sws_scale(pSWSCtx, _pInputFrame->data, _pInputFrame->linesize, 0, m_pCodecCtx->height, _pOutputFrame->data, _pOutputFrame->linesize); 
		ppOutputData = _pInputFrame->data;
		piStride = _pInputFrame->linesize;
	}

	try
	{
		sws_scale(pSWSCtx, ppOutputData, piStride, 0, m_pCodecCtx->height, _pOutputFrame->data, _pOutputFrame->linesize); 
	}
	catch(Exception^)
	{
		bSuccess = false;
		log->Error("RescaleAndConvert Error : sws_scale failed.");
	}

	// Clean Up.
	sws_freeContext(pSWSCtx);
	
	if(pDeinterlaceBuffer != nullptr)
	{
		delete []pDeinterlaceBuffer;
		pDeinterlaceBuffer = nullptr;
	}

	return bSuccess;

}
void VideoFile::SetImageGeometry()
{
	// Set the image geometry according to the pixel aspect ratio choosen.

	// Image height. (width never moves)
	switch(m_InfosVideo->eAspectRatio)
	{
		case AspectRatio::Force43:
			log->Debug("Image aspect ratio is on : Force 4:3.");
			m_InfosVideo->iDecodingHeight = (int)(((double)m_InfosVideo->iWidth * 3) / 4);
			break;
		case AspectRatio::Force169:
			log->Debug("Image aspect ratio is on : Force 16:9.");
			m_InfosVideo->iDecodingHeight = (int)(((double)m_InfosVideo->iWidth * 9) / 16);
			break;
		case AspectRatio::ForceSquarePixels:
			log->Debug("Image aspect ratio is on : Force square pixels.");
			m_InfosVideo->iDecodingHeight = m_InfosVideo->iHeight;
			break;
		case AspectRatio::AutoDetect:
		default:
			log->Debug("Image aspect ratio is on : AutoDetect.");
			// fPixelAspectRatio Might be 1.0f if square pixels.
			m_InfosVideo->iDecodingHeight = (int)((double)m_InfosVideo->iHeight / m_InfosVideo->fPixelAspectRatio);
			break;
	}

	// Fix unsupported width.
	if(m_InfosVideo->iWidth % 4 != 0)
	{
		m_InfosVideo->iDecodingWidth = 4 * ((m_InfosVideo->iWidth / 4) + 1);
	}
	else
	{
		m_InfosVideo->iDecodingWidth = m_InfosVideo->iWidth;
	}


	// Reallocate buffer.		
	delete [] m_Buffer;
	m_Buffer = nullptr;

	int iSizeBuffer = avpicture_get_size(PIX_FMT_BGR24, m_InfosVideo->iDecodingWidth, m_InfosVideo->iDecodingHeight);									
	m_Buffer = new uint8_t[iSizeBuffer];

	// Are next 2 lines needed here ?
	av_free(m_pCurrentDecodedFrameBGR);						
	m_pCurrentDecodedFrameBGR = avcodec_alloc_frame();
	
	// Assign appropriate parts of buffer to image planes in pFrameBGR
	avpicture_fill((AVPicture *)m_pCurrentDecodedFrameBGR, m_Buffer , PIX_FMT_BGR24, m_InfosVideo->iDecodingWidth, m_InfosVideo->iDecodingHeight);
}
}	
}