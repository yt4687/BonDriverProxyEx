#define _CRT_SECURE_NO_WARNINGS
#include "BonDriverProxyEx.h"

#if _DEBUG
static cProxyServerEx *debug;
#endif

static std::list<cProxyServerEx *> InstanceList;
static cCriticalSection Lock_Instance;

cProxyServerEx::cProxyServerEx() : m_Error(TRUE, FALSE)
{
	m_s = INVALID_SOCKET;
	m_hModule = NULL;
	m_pIBon = m_pIBon2 = m_pIBon3 = NULL;
	m_bTunerOpen = FALSE;
	m_hTsRead = NULL;
	m_pStopTsRead = NULL;
	m_pTsLock = NULL;
	m_ppos = NULL;
	m_dwSpace = m_dwChannel = 0xff;
	m_pDriversMapKey = NULL;
	m_iDriverNo = -1;
}

cProxyServerEx::~cProxyServerEx()
{
	{
		BOOL bRelease = TRUE;
		LOCK(Lock_Instance);
		std::list<cProxyServerEx *>::iterator it = InstanceList.begin();
		while (it != InstanceList.end())
		{
			if (*it == this)
				InstanceList.erase(it++);
			else
			{
				if ((m_hModule != NULL) && (m_hModule == (*it)->m_hModule))
					bRelease = FALSE;
				++it;
			}
		}
		if (bRelease)
		{
			if (m_hTsRead)
			{
				*m_pStopTsRead = TRUE;
				::WaitForSingleObject(m_hTsRead, INFINITE);
				::CloseHandle(m_hTsRead);
				delete m_pStopTsRead;
				delete m_pTsLock;
				delete m_ppos;
			}
			if (m_pIBon)
				m_pIBon->Release();
			if (m_hModule)
			{
				stDrivers *pstDrivers = DriversMap[m_pDriversMapKey];
				pstDrivers[m_iDriverNo].bUsed = FALSE;
				::FreeLibrary(m_hModule);
			}
		}
	}

	if (m_s != INVALID_SOCKET)
		::closesocket(m_s);
}

DWORD WINAPI cProxyServerEx::Reception(LPVOID pv)
{
	cProxyServerEx *pProxy = static_cast<cProxyServerEx *>(pv);
	DWORD ret = pProxy->Process();
#if _DEBUG
	debug = NULL;
#endif
	delete pProxy;
	return ret;
}

DWORD cProxyServerEx::Process()
{
	HANDLE hThread[2];
	hThread[0] = ::CreateThread(NULL, 0, cProxyServerEx::Sender, this, 0, NULL);
	if (hThread[0] == NULL)
		return 1;

	hThread[1] = ::CreateThread(NULL, 0, cProxyServerEx::Receiver, this, 0, NULL);
	if (hThread[1] == NULL)
	{
		m_Error.Set();
		::WaitForSingleObject(hThread[0], INFINITE);
		::CloseHandle(hThread[0]);
		return 2;
	}

#if _DEBUG
	debug = this;
#endif

	LPVOID ppv[4];
	HANDLE h[2] = { m_Error, m_fifoRecv.GetEventHandle() };
	while (1)
	{
		DWORD dwRet = ::WaitForMultipleObjects(2, h, FALSE, INFINITE);
		switch (dwRet)
		{
		case WAIT_OBJECT_0:
			goto end;

		case WAIT_OBJECT_0 + 1:
		{
			cPacketHolder *pPh;
			m_fifoRecv.Pop(&pPh);
			switch (pPh->GetCommand())
			{
			case eSelectBonDriver:
			{
				if (pPh->GetBodyLength() <= sizeof(char))
					makePacket(eSelectBonDriver, FALSE);
				else
				{
					BOOL b;
					{
						LOCK(Lock_Instance);
						b = SelectBonDriver((LPCSTR)(pPh->m_pPacket->payload));
						if (b)
							InstanceList.push_back(this);
					}
					makePacket(eSelectBonDriver, b);
				}
				break;
			}

			case eCreateBonDriver:
			{
				if (m_pIBon == NULL)
				{
					BOOL bFind = FALSE;
					{
						LOCK(Lock_Instance);
						for (std::list<cProxyServerEx *>::iterator it = InstanceList.begin(); it != InstanceList.end(); ++it)
						{
							if (*it == this)
								continue;
							if (m_hModule == (*it)->m_hModule)
							{
								if ((*it)->m_pIBon != NULL)
								{
									bFind = TRUE;	// ここに来るのはかなりのレアケースのハズ
									m_pIBon = (*it)->m_pIBon;
									m_pIBon2 = (*it)->m_pIBon2;
									m_pIBon3 = (*it)->m_pIBon3;
									break;
								}
								// ここに来るのは上より更にレアケース
								// 一応リストの最後まで検索してみて、それでも見つからなかったら
								// CreateBonDriver()をやらせてみる
							}
						}
					}
					if (!bFind)
					{
						if ((CreateBonDriver() != NULL) && (m_pIBon2 != NULL))
							makePacket(eCreateBonDriver, TRUE);
						else
						{
							makePacket(eCreateBonDriver, FALSE);
							m_Error.Set();
						}
					}
					else
						makePacket(eCreateBonDriver, TRUE);
				}
				else
					makePacket(eCreateBonDriver, TRUE);
				break;
			}

			case eOpenTuner:
			{
				BOOL bFind = FALSE;
				{
					LOCK(Lock_Instance);
					for (std::list<cProxyServerEx *>::iterator it = InstanceList.begin(); it != InstanceList.end(); ++it)
					{
						if (*it == this)
							continue;
						if ((m_pIBon != NULL) && (m_pIBon == (*it)->m_pIBon))
						{
							if ((*it)->m_bTunerOpen)
							{
								bFind = TRUE;
								m_bTunerOpen = TRUE;
								break;
							}
						}
					}
				}
				if (!bFind)
					m_bTunerOpen = OpenTuner();
				makePacket(eOpenTuner, m_bTunerOpen);
				break;
			}

			case eCloseTuner:
			{
				BOOL bFind = FALSE;
				{
					LOCK(Lock_Instance);
					for (std::list<cProxyServerEx *>::iterator it = InstanceList.begin(); it != InstanceList.end(); ++it)
					{
						if (*it == this)
							continue;
						if ((m_pIBon != NULL) && (m_pIBon == (*it)->m_pIBon))
						{
							if ((*it)->m_bTunerOpen)
							{
								bFind = TRUE;
								break;
							}
						}
					}
				}
				if (!bFind)
				{
					if (m_pTsLock != NULL)
					{
						LOCK(*m_pTsLock);
						CloseTuner();
						*m_ppos = 0;
						m_bTunerOpen = FALSE;
					}
				}
				break;
			}

			case ePurgeTsStream:
			{
				if (m_bChannelLock && (m_pTsLock != NULL))
				{
					LOCK(*m_pTsLock);
					PurgeTsStream();
					*m_ppos = 0;
					makePacket(ePurgeTsStream, TRUE);
				}
				else
					makePacket(ePurgeTsStream, FALSE);
				break;
			}

			case eRelease:
				m_Error.Set();
				break;

			case eEnumTuningSpace:
			{
				if (pPh->GetBodyLength() != sizeof(DWORD))
					makePacket(eEnumTuningSpace, _T(""));
				else
				{
					LPCTSTR p = EnumTuningSpace(::ntohl(*(DWORD *)(pPh->m_pPacket->payload)));
					if (p)
						makePacket(eEnumTuningSpace, p);
					else
						makePacket(eEnumTuningSpace, _T(""));
				}
				break;
			}

			case eEnumChannelName:
			{
				if (pPh->GetBodyLength() != (sizeof(DWORD) * 2))
					makePacket(eEnumChannelName, _T(""));
				else
				{
					LPCTSTR p = EnumChannelName(::ntohl(*(DWORD *)(pPh->m_pPacket->payload)), ::ntohl(*(DWORD *)&(pPh->m_pPacket->payload[sizeof(DWORD)])));
					if (p)
						makePacket(eEnumChannelName, p);
					else
						makePacket(eEnumChannelName, _T(""));
				}
				break;
			}

			case eSetChannel2:
			{
				if (pPh->GetBodyLength() != ((sizeof(DWORD) * 2) + sizeof(BYTE)))
					makePacket(eSetChannel2, (DWORD)0xff);
				else
				{
					m_bChannelLock = pPh->m_pPacket->payload[sizeof(DWORD) * 2];
					DWORD dwReqSpace = ::ntohl(*(DWORD *)(pPh->m_pPacket->payload));
					DWORD dwReqChannel = ::ntohl(*(DWORD *)&(pPh->m_pPacket->payload[sizeof(DWORD)]));
					BOOL bLocked = FALSE;
					BOOL bChanged = FALSE;
					BOOL bShared = FALSE;
					{
						LOCK(Lock_Instance);
						for (std::list<cProxyServerEx *>::iterator it = InstanceList.begin(); it != InstanceList.end(); ++it)
						{
							if (*it == this)
								continue;
							// ひとまず現在のインスタンスが共有されているかどうかを確認しておく
							if ((m_pIBon != NULL) && (m_pIBon == (*it)->m_pIBon))
								bShared = TRUE;

							// 対象BonDriver群の中でチューナをオープンしているもの
							if (m_pDriversMapKey == (*it)->m_pDriversMapKey && (*it)->m_pIBon != NULL && (*it)->m_bTunerOpen)
							{
								// かつクライアントからの要求と同一チャンネルを選択しているもの
								if ((*it)->m_dwSpace == dwReqSpace && (*it)->m_dwChannel == dwReqChannel)
								{
									// 今クライアントがオープンしているチューナに関して
									if (m_pIBon != NULL)
									{
										BOOL bModule = FALSE;
										BOOL bIBon = FALSE;
										BOOL bTuner = FALSE;
										for (std::list<cProxyServerEx *>::iterator it2 = InstanceList.begin(); it2 != InstanceList.end(); ++it2)
										{
											if (*it2 == this)
												continue;
											if ((m_hModule == (*it2)->m_hModule))
											{
												bModule = TRUE;	// モジュール使用者有り
												if ((m_pIBon == (*it2)->m_pIBon))
												{
													bIBon = TRUE;	// インスタンス使用者有り
													if ((*it2)->m_bTunerOpen)
													{
														bTuner = TRUE;	// チューナ使用者有り
														break;
													}
												}
											}
										}
										// チューナ使用者無しならクローズ
										if (!bTuner)
										{
											if (m_hTsRead)
											{
												*m_pStopTsRead = TRUE;
												::WaitForSingleObject(m_hTsRead, INFINITE);
												::CloseHandle(m_hTsRead);
												m_hTsRead = NULL;
												delete m_pStopTsRead;
												m_pStopTsRead = NULL;
												delete m_pTsLock;
												m_pTsLock = NULL;
												delete m_ppos;
												m_ppos = NULL;
											}
											CloseTuner();
											// かつインスタンス使用者も無しならインスタンスリリース
											if (!bIBon)
											{
												m_pIBon->Release();
												// かつモジュール使用者も無しならモジュールリリース
												if (!bModule)
												{
													stDrivers *pstDrivers = DriversMap[m_pDriversMapKey];
													pstDrivers[m_iDriverNo].bUsed = FALSE;
													::FreeLibrary(m_hModule);
												}
											}
										}
									}
									// インスタンス切り替え
									bChanged = TRUE;
									m_hModule = (*it)->m_hModule;
									m_iDriverNo = (*it)->m_iDriverNo;
									m_pIBon = (*it)->m_pIBon;
									m_pIBon2 = (*it)->m_pIBon2;
									m_pIBon3 = (*it)->m_pIBon3;
									m_bTunerOpen = TRUE;
									m_dwSpace = dwReqSpace;
									m_dwChannel = dwReqChannel;
									m_hTsRead = (*it)->m_hTsRead;
									m_pStopTsRead = (*it)->m_pStopTsRead;
									m_pTsLock = (*it)->m_pTsLock;
									m_ppos = (*it)->m_ppos;
#if _DEBUG && DETAILLOG2
									_RPT3(_CRT_WARN, "** found! ** : m_hModule[%p] / m_iDriverNo[%d] / m_pIBon[%p]\n", m_hModule, m_iDriverNo, m_pIBon);
									_RPT3(_CRT_WARN, "             : m_dwSpace[%d] / m_dwChannel[%d] / m_bChannelLock[%d]\n", m_dwSpace, m_dwChannel, m_bChannelLock);
#endif
									goto ok;	// これは酷い
								}
							}
						}

						// 同一チャンネルを使用中のチューナは見つからず、現在のチューナは共有されていたら
						if (bShared)
						{
							IBonDriver *old_pIBon = m_pIBon;
							BOOL old_bTunerOpen = m_bTunerOpen;
							// 出来れば未使用、無理ならなるべくロックされてないチューナを選択して、
							// 一気にチューナオープン状態にまで持って行く
							if (SelectBonDriver(m_pDriversMapKey))
							{
								if (m_pIBon == NULL)
								{
									// 未使用チューナがあった
									if ((CreateBonDriver() == NULL) || (m_pIBon2 == NULL))
									{
										makePacket(eSetChannel2, (DWORD)0xff);
										m_Error.Set();
										break;
									}
								}
								else
								{
									// インスタンス切り替えか？
									if (m_pIBon != old_pIBon)
										bChanged = TRUE;
									else
										m_bTunerOpen = old_bTunerOpen;
								}
								if (!m_bTunerOpen)
								{
									m_bTunerOpen = OpenTuner();
									if (!m_bTunerOpen)
									{
										makePacket(eSetChannel2, (DWORD)0xff);
										m_Error.Set();
										break;
									}
								}
							}
							else
							{
								makePacket(eSetChannel2, (DWORD)0xff);
								m_Error.Set();
								break;
							}
						}

						// 使用チューナのチャンネルロック状態確認
						for (std::list<cProxyServerEx *>::iterator it = InstanceList.begin(); it != InstanceList.end(); ++it)
						{
							if (*it == this)
								continue;
							if ((m_pIBon != NULL) && (m_pIBon == (*it)->m_pIBon))
							{
								if ((*it)->m_bChannelLock)
								{
									bLocked = TRUE;
									break;
								}
							}
						}

#if _DEBUG && DETAILLOG2
						_RPT3(_CRT_WARN, "eSetChannel2 : bShared[%d] / bLocked[%d] / bChanged[%d]\n", bShared, bLocked, bChanged);
						_RPT3(_CRT_WARN, "             : dwReqSpace[%d] / dwReqChannel[%d] / m_bChannelLock[%d]\n", dwReqSpace, dwReqChannel, m_bChannelLock);
#endif

					}
					if (bLocked)
					{
						// ロックされてる時は単純にロックされてる事を通知
						// この場合クライアントアプリへのSetChannel()の戻り値は成功になる
						// (おそらく致命的な問題にはならない)
						makePacket(eSetChannel2, (DWORD)0x01);
					}
					else
					{
						if (m_pTsLock != NULL)
							m_pTsLock->Enter();
						BOOL b = SetChannel(dwReqSpace, dwReqChannel);
						if (m_pTsLock != NULL)
							m_pTsLock->Leave();
						if (b)
						{
							m_dwSpace = dwReqSpace;
							m_dwChannel = dwReqChannel;
						ok:
							makePacket(eSetChannel2, (DWORD)0x00);
							if (m_hTsRead == NULL)
							{
								BOOL bFind = FALSE;
								LOCK(Lock_Instance);
								for (std::list<cProxyServerEx *>::iterator it = InstanceList.begin(); it != InstanceList.end(); ++it)
								{
									if (*it == this)
										continue;
									if ((m_pIBon != NULL) && (m_pIBon == (*it)->m_pIBon))
									{
										if ((*it)->m_hTsRead != NULL)
										{
											bFind = TRUE;
											m_hTsRead = (*it)->m_hTsRead;
											m_pStopTsRead = (*it)->m_pStopTsRead;
											m_pTsLock = (*it)->m_pTsLock;
											m_ppos = (*it)->m_ppos;
											break;
										}
									}
								}
								if (!bFind)
								{
									m_pStopTsRead = new BOOL(FALSE);
									m_pTsLock = new cCriticalSection();
									m_ppos = new DWORD(0);
									ppv[0] = m_pIBon;
									ppv[1] = m_pStopTsRead;
									ppv[2] = m_pTsLock;
									ppv[3] = m_ppos;
									m_hTsRead = ::CreateThread(NULL, 0, cProxyServerEx::TsReader, ppv, 0, NULL);
									if (m_hTsRead == NULL)
									{
										delete m_pStopTsRead;
										m_pStopTsRead = NULL;
										delete m_pTsLock;
										m_pTsLock = NULL;
										delete m_ppos;
										m_ppos = NULL;
										m_Error.Set();
									}
								}
							}
							else
							{
								// インスタンス切り替えだった場合は既存のTSバッファに介入しない
								if (!bChanged)
								{
									LOCK(*m_pTsLock);
									*m_ppos = 0;
								}
							}
						}
						else
							makePacket(eSetChannel2, (DWORD)0xff);
					}
				}
				break;
			}

			case eGetTotalDeviceNum:
				makePacket(eGetTotalDeviceNum, GetTotalDeviceNum());
				break;

			case eGetActiveDeviceNum:
				makePacket(eGetActiveDeviceNum, GetActiveDeviceNum());
				break;

			case eSetLnbPower:
			{
				if (pPh->GetBodyLength() != sizeof(BYTE))
					makePacket(eSetLnbPower, FALSE);
				else
					makePacket(eSetLnbPower, SetLnbPower((BOOL)(pPh->m_pPacket->payload[0])));
				break;
			}

			default:
				break;
			}
			delete pPh;
			break;
		}

		default:
			// 何かのエラー
			m_Error.Set();
			goto end;
		}
	}
end:
	::WaitForMultipleObjects(2, hThread, TRUE, INFINITE);
	::CloseHandle(hThread[0]);
	::CloseHandle(hThread[1]);
	return 0;
}

int cProxyServerEx::ReceiverHelper(char *pDst, int left)
{
	int len, ret;
	fd_set rd;
	timeval tv;

	tv.tv_sec = 1;
	tv.tv_usec = 0;
	while (left > 0)
	{
		if (m_Error.IsSet())
			return -1;

		FD_ZERO(&rd);
		FD_SET(m_s, &rd);
		if ((len = ::select((int)(m_s + 1), &rd, NULL, NULL, &tv)) == SOCKET_ERROR)
		{
			ret = -2;
			goto err;
		}

		if (len == 0)
			continue;

		if ((len = ::recv(m_s, pDst, left, 0)) == SOCKET_ERROR)
		{
			ret = -3;
			goto err;
		}
		else if (len == 0)
		{
			ret = -4;
			goto err;
		}
		left -= len;
		pDst += len;
	}
	return 0;
err:
	m_Error.Set();
	return ret;
}

DWORD WINAPI cProxyServerEx::Receiver(LPVOID pv)
{
	cProxyServerEx *pProxy = static_cast<cProxyServerEx *>(pv);
	int left;
	char *p;
	DWORD ret;
	cPacketHolder *pPh = NULL;

	while (1)
	{
		pPh = new cPacketHolder(16);
		left = sizeof(stPacketHead);
		p = (char *)&(pPh->m_pPacket->head);
		if (pProxy->ReceiverHelper(p, left) != 0)
		{
			ret = 201;
			goto end;
		}

		if (!pPh->IsValid())
		{
			pProxy->m_Error.Set();
			ret = 202;
			goto end;
		}

		left = (int)pPh->GetBodyLength();
		if (left == 0)
		{
			pProxy->m_fifoRecv.Push(pPh);
			continue;
		}

		if (left > 512 || left < 0)
		{
			pProxy->m_Error.Set();
			ret = 203;
			goto end;
		}

		if (left >= 16)
		{
			cPacketHolder *pTmp = new cPacketHolder(left);
			pTmp->m_pPacket->head = pPh->m_pPacket->head;
			delete pPh;
			pPh = pTmp;
		}

		p = (char *)(pPh->m_pPacket->payload);
		if (pProxy->ReceiverHelper(p, left) != 0)
		{
			ret = 204;
			goto end;
		}

		pProxy->m_fifoRecv.Push(pPh);
	}
end:
	if (pPh)
		delete pPh;
	return ret;
}

void cProxyServerEx::makePacket(enumCommand eCmd, BOOL b)
{
	cPacketHolder *p = new cPacketHolder(eCmd, sizeof(BYTE));
	p->m_pPacket->payload[0] = (BYTE)b;
	m_fifoSend.Push(p);
}

void cProxyServerEx::makePacket(enumCommand eCmd, DWORD dw)
{
	cPacketHolder *p = new cPacketHolder(eCmd, sizeof(DWORD));
	DWORD *pos = (DWORD *)(p->m_pPacket->payload);
	*pos = ::htonl(dw);
	m_fifoSend.Push(p);
}

void cProxyServerEx::makePacket(enumCommand eCmd, LPCTSTR str)
{
	register size_t size = (::_tcslen(str) + 1) * sizeof(TCHAR);
	cPacketHolder *p = new cPacketHolder(eCmd, size);
	::memcpy(p->m_pPacket->payload, str, size);
	m_fifoSend.Push(p);
}

void cProxyServerEx::makePacket(enumCommand eCmd, BYTE *pSrc, DWORD dwSize, float fSignalLevel)
{
	register size_t size = (sizeof(DWORD) * 2) + dwSize;
	cPacketHolder *p = new cPacketHolder(eCmd, size);
	union {
		DWORD dw;
		float f;
	} u;
	u.f = fSignalLevel;
	DWORD *pos = (DWORD *)(p->m_pPacket->payload);
	*pos++ = ::htonl(dwSize);
	*pos++ = ::htonl(u.dw);
	if (dwSize > 0)
		::memcpy(pos, pSrc, dwSize);
	m_fifoSend.Push(p);
}

DWORD WINAPI cProxyServerEx::Sender(LPVOID pv)
{
	cProxyServerEx *pProxy = static_cast<cProxyServerEx *>(pv);
	DWORD ret;
	HANDLE h[2] = { pProxy->m_Error, pProxy->m_fifoSend.GetEventHandle() };
	while (1)
	{
		DWORD dwRet = ::WaitForMultipleObjects(2, h, FALSE, INFINITE);
		switch (dwRet)
		{
		case WAIT_OBJECT_0:
			ret = 101;
			goto end;

		case WAIT_OBJECT_0 + 1:
		{
			cPacketHolder *pPh;
			pProxy->m_fifoSend.Pop(&pPh);
			int left = (int)pPh->m_Size;
			char *p = (char *)(pPh->m_pPacket);
			while (left > 0)
			{
				int len = ::send(pProxy->m_s, p, left, 0);
				if (len == SOCKET_ERROR)
				{
					pProxy->m_Error.Set();
					break;
				}
				left -= len;
				p += len;
			}
			delete pPh;
			break;
		}

		default:
			// 何かのエラー
			pProxy->m_Error.Set();
			ret = 102;
			goto end;
		}
	}
end:
	return ret;
}

DWORD WINAPI cProxyServerEx::TsReader(LPVOID pv)
{
	LPVOID *ppv = static_cast<LPVOID *>(pv);
	IBonDriver *pIBon = static_cast<IBonDriver *>(ppv[0]);
	volatile BOOL &StopTsRead = *(static_cast<BOOL *>(ppv[1]));
	cCriticalSection &TsLock = *(static_cast<cCriticalSection *>(ppv[2]));
	DWORD &pos = *(static_cast<DWORD *>(ppv[3]));
	DWORD dwSize, dwRemain, now, before = 0;
	float fSignalLevel = 0;
	DWORD ret = 300;
	const DWORD TsPacketBufSize = g_TsPacketBufSize;
	BYTE *pBuf, *pTsBuf = new BYTE[TsPacketBufSize];
#if _DEBUG && DETAILLOG
	DWORD Counter = 0;
#endif

	// TS読み込みループ
	while (!StopTsRead)
	{
		if (((now = ::GetTickCount()) - before) >= 1000)
		{
			fSignalLevel = pIBon->GetSignalLevel();
			before = now;
		}
		dwSize = dwRemain = 0;
		{
			LOCK(TsLock);
			if (pIBon->GetTsStream(&pBuf, &dwSize, &dwRemain) && (dwSize != 0))
			{
				if ((pos + dwSize) < TsPacketBufSize)
				{
					::memcpy(&pTsBuf[pos], pBuf, dwSize);
					pos += dwSize;
					if (dwRemain == 0)
					{
						{
							LOCK(Lock_Instance);
							for (std::list<cProxyServerEx *>::iterator it = InstanceList.begin(); it != InstanceList.end(); ++it)
							{
								if (pIBon == (*it)->m_pIBon)
									(*it)->makePacket(eGetTsStream, pTsBuf, pos, fSignalLevel);
							}
#if _DEBUG && DETAILLOG
							_RPT3(_CRT_WARN, "makePacket0() : %u : size[%x] / dwRemain[%d]\n", Counter++, pos, dwRemain);
#endif
						}
						pos = 0;
					}
				}
				else
				{
					DWORD left, dwLen = TsPacketBufSize - pos;
					::memcpy(&pTsBuf[pos], pBuf, dwLen);
					{
						LOCK(Lock_Instance);
						for (std::list<cProxyServerEx *>::iterator it = InstanceList.begin(); it != InstanceList.end(); ++it)
						{
							if (pIBon == (*it)->m_pIBon)
								(*it)->makePacket(eGetTsStream, pTsBuf, TsPacketBufSize, fSignalLevel);
						}
#if _DEBUG && DETAILLOG
						_RPT3(_CRT_WARN, "makePacket1() : %u : size[%x] / dwRemain[%d]\n", Counter++, TsPacketBufSize, dwRemain);
#endif
						left = dwSize - dwLen;
						pBuf += dwLen;
						while (left > TsPacketBufSize)
						{
							for (std::list<cProxyServerEx *>::iterator it = InstanceList.begin(); it != InstanceList.end(); ++it)
							{
								if (pIBon == (*it)->m_pIBon)
									(*it)->makePacket(eGetTsStream, pBuf, TsPacketBufSize, fSignalLevel);
							}
#if _DEBUG && DETAILLOG
							_RPT2(_CRT_WARN, "makePacket2() : %u : size[%x]\n", Counter++, TsPacketBufSize);
#endif
							left -= TsPacketBufSize;
							pBuf += TsPacketBufSize;
						}
					}
					if (left != 0)
					{
						if (dwRemain == 0)
						{
							LOCK(Lock_Instance);
							for (std::list<cProxyServerEx *>::iterator it = InstanceList.begin(); it != InstanceList.end(); ++it)
							{
								if (pIBon == (*it)->m_pIBon)
									(*it)->makePacket(eGetTsStream, pBuf, left, fSignalLevel);
							}
#if _DEBUG && DETAILLOG
							_RPT3(_CRT_WARN, "makePacket3() : %u : size[%x] / dwRemain[%d]\n", Counter++, left, dwRemain);
#endif
							left = 0;
						}
						else
							::memcpy(pTsBuf, pBuf, left);
					}
					pos = left;
				}
			}
		}
		if (dwRemain == 0)
			::Sleep(WAIT_TIME);
	}
	delete[] pTsBuf;
	return ret;
}

BOOL cProxyServerEx::SelectBonDriver(LPCSTR p)
{
	m_hModule = NULL;
	char *pKey = NULL;
	stDrivers *pstDrivers = NULL;
	for (std::map<char *, stDrivers *>::iterator it = DriversMap.begin(); it != DriversMap.end(); ++it)
	{
		if (::strcmp(p, it->first) == 0)
		{
			pKey = it->first;
			pstDrivers = it->second;
			break;
		}
	}
	if (pstDrivers == NULL)
		return FALSE;

	// まず使われてないのを探す
	for (int i = 0; pstDrivers[i].strBonDriver != NULL; i++)
	{
		if (pstDrivers[i].bUsed)
			continue;
		m_hModule = ::LoadLibraryA(pstDrivers[i].strBonDriver);
		if (m_hModule != NULL)
		{
			pstDrivers[i].bUsed = TRUE;
			m_pDriversMapKey = pKey;
			m_iDriverNo = i;
			// eSetChannel2からも呼ばれるので、各種項目再初期化
			m_pIBon = m_pIBon2 = m_pIBon3 = NULL;
			m_bTunerOpen = FALSE;
			m_hTsRead = NULL;
			m_pStopTsRead = NULL;
			m_pTsLock = NULL;
			m_ppos = NULL;
			return TRUE;
		}
	}

	// 全部使われてたら(あるいはLoadLibrary()出来なければ)、チャンネルロックされてないのを優先で選択
	for (std::list<cProxyServerEx *>::iterator it = InstanceList.begin(); it != InstanceList.end(); ++it)
	{
		if (*it == this)
			continue;
		if (::strcmp(p, (*it)->m_pDriversMapKey) == 0)	// ひとまず候補
		{
			m_hModule = (*it)->m_hModule;
			m_pDriversMapKey = (*it)->m_pDriversMapKey;
			m_iDriverNo = (*it)->m_iDriverNo;
			m_pIBon = (*it)->m_pIBon;	// (*it)->m_pIBonがNULLの可能性はゼロではない
			m_pIBon2 = (*it)->m_pIBon2;
			m_pIBon3 = (*it)->m_pIBon3;
			m_bTunerOpen = (*it)->m_bTunerOpen;
			m_hTsRead = (*it)->m_hTsRead;
			m_pStopTsRead = (*it)->m_pStopTsRead;
			m_pTsLock = (*it)->m_pTsLock;
			m_ppos = (*it)->m_ppos;
			// かなりダサいけど多分現実的に問題になる程ではないので良しとする…
			BOOL bLocked = FALSE;
			for (std::list<cProxyServerEx *>::iterator it2 = InstanceList.begin(); it2 != InstanceList.end(); ++it2)
			{
				if (*it2 == this)
					continue;
				if (m_hModule == (*it2)->m_hModule)
				{
					if ((*it2)->m_bChannelLock)	// ロックされてた
						bLocked = TRUE;
				}
			}
			if (!bLocked)	// ロックされてなければ即決定
				break;
		}
	}

	return (m_hModule != NULL);
}

IBonDriver *cProxyServerEx::CreateBonDriver()
{
	if (m_hModule)
	{
		IBonDriver *(*f)() = (IBonDriver *(*)())::GetProcAddress(m_hModule, "CreateBonDriver");
		if (f)
		{
			try { m_pIBon = f(); }
			catch (...) {}
			if (m_pIBon)
			{
				m_pIBon2 = dynamic_cast<IBonDriver2 *>(m_pIBon);
				m_pIBon3 = dynamic_cast<IBonDriver3 *>(m_pIBon);
			}
		}
	}
	return m_pIBon;
}

const BOOL cProxyServerEx::OpenTuner(void)
{
	BOOL b = FALSE;
	if (m_pIBon)
		b = m_pIBon->OpenTuner();
	return b;
}

void cProxyServerEx::CloseTuner(void)
{
	if (m_pIBon)
		m_pIBon->CloseTuner();
}

void cProxyServerEx::PurgeTsStream(void)
{
	if (m_pIBon)
		m_pIBon->PurgeTsStream();
}

LPCTSTR cProxyServerEx::EnumTuningSpace(const DWORD dwSpace)
{
	LPCTSTR pStr = NULL;
	if (m_pIBon2)
		pStr = m_pIBon2->EnumTuningSpace(dwSpace);
	return pStr;
}

LPCTSTR cProxyServerEx::EnumChannelName(const DWORD dwSpace, const DWORD dwChannel)
{
	LPCTSTR pStr = NULL;
	if (m_pIBon2)
		pStr = m_pIBon2->EnumChannelName(dwSpace, dwChannel);
	return pStr;
}

const BOOL cProxyServerEx::SetChannel(const DWORD dwSpace, const DWORD dwChannel)
{
	BOOL b = FALSE;
	if (m_pIBon2)
		b = m_pIBon2->SetChannel(dwSpace, dwChannel);
	return b;
}

const DWORD cProxyServerEx::GetTotalDeviceNum(void)
{
	DWORD d = 0;
	if (m_pIBon3)
		d = m_pIBon3->GetTotalDeviceNum();
	return d;
}

const DWORD cProxyServerEx::GetActiveDeviceNum(void)
{
	DWORD d = 0;
	if (m_pIBon3)
		d = m_pIBon3->GetActiveDeviceNum();
	return d;
}

const BOOL cProxyServerEx::SetLnbPower(const BOOL bEnable)
{
	BOOL b = FALSE;
	if (m_pIBon3)
		b = m_pIBon3->SetLnbPower(bEnable);
	return b;
}

#if _DEBUG
struct HostInfo{
	char *host;
	unsigned short port;
};
DWORD WINAPI Listen(LPVOID pv)
{
	HostInfo *hinfo = static_cast<HostInfo *>(pv);
	char *host = hinfo->host;
	unsigned short port = hinfo->port;
#else
int Listen(char *host, unsigned short port)
{
#endif
	SOCKADDR_IN address;
	LPHOSTENT he;
	SOCKET lsock, csock;

	lsock = socket(AF_INET, SOCK_STREAM, 0);
	if (lsock == INVALID_SOCKET)
		return 1;

	BOOL reuse = TRUE;
	setsockopt(lsock, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse, sizeof(reuse));
	memset((char *)&address, 0, sizeof(address));
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = inet_addr(host);
	if (address.sin_addr.s_addr == INADDR_NONE)
	{
		he = gethostbyname(host);
		if (he == NULL)
		{
			closesocket(lsock);
			return 2;
		}
		memcpy(&(address.sin_addr), *(he->h_addr_list), he->h_length);
	}
	address.sin_port = htons(port);
	if (bind(lsock, (LPSOCKADDR)&address, sizeof(address)) == SOCKET_ERROR)
	{
		closesocket(lsock);
		return 3;
	}
	if (listen(lsock, 4) == SOCKET_ERROR)
	{
		closesocket(lsock);
		return 4;
	}

	while (1)
	{
		int len = sizeof(address);
		csock = accept(lsock, (LPSOCKADDR)&address, &len);
		if (csock == INVALID_SOCKET)
			continue;

		cProxyServerEx *pProxy = new cProxyServerEx();
		pProxy->setSocket(csock);
		HANDLE hThread = ::CreateThread(NULL, 0, cProxyServerEx::Reception, pProxy, 0, NULL);
		if (hThread)
			CloseHandle(hThread);
		else
			delete pProxy;
	}

	return 0;	// ここには来ない
}

#if _DEBUG
LRESULT CALLBACK WndProc(HWND hWnd, UINT iMsg, WPARAM wParam, LPARAM lParam)
{
	HDC hDc;
	TCHAR buf[1024];
	static int n = 0;

	switch (iMsg)
	{
	case WM_CREATE:
		return 0;

	case WM_DESTROY:
		PostQuitMessage(0);
		return 0;

	case WM_PAINT:
		PAINTSTRUCT ps;
		hDc = BeginPaint(hWnd, &ps);
		wsprintf(buf, TEXT("pProxy [%p] n[%d]"), debug, n);
		TextOut(hDc, 0, 0, buf, (int)_tcslen(buf));
		if (debug)
		{
			wsprintf(buf, TEXT("send_fifo size[%d] recv_fifo size[%d]"), debug->m_fifoSend.Size(), debug->m_fifoRecv.Size());
			TextOut(hDc, 0, 40, buf, (int)_tcslen(buf));
		}
		EndPaint(hWnd, &ps);
		return 0;

	case WM_LBUTTONDOWN:
		n++;
		InvalidateRect(hWnd, NULL, FALSE);
		return 0;
	}
	return DefWindowProc(hWnd, iMsg, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
{
	static HANDLE hLogFile = CreateFile(_T("dbglog.txt"), GENERIC_WRITE, FILE_SHARE_WRITE, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	_CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
	_CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE | _CRTDBG_MODE_DEBUG);
	_CrtSetReportFile(_CRT_WARN, hLogFile);
	_CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE | _CRTDBG_MODE_DEBUG);
	_CrtSetReportFile(_CRT_ERROR, hLogFile);
	_RPT0(_CRT_WARN, "--- PROCESS_START ---\n");
//	int *p = new int[2];	// リーク検出テスト用

	if (Init(hInstance) != 0)
		return -1;

	WSADATA wsa;
	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
		return -2;

	HostInfo hinfo;
	hinfo.host = g_Host;
	hinfo.port = g_Port;
	HANDLE hThread = CreateThread(NULL, 0, Listen, &hinfo, 0, NULL);
	CloseHandle(hThread);

	HWND hWnd;
	MSG msg;
	WNDCLASSEX wndclass;

	wndclass.cbSize = sizeof(wndclass);
	wndclass.style = CS_HREDRAW | CS_VREDRAW;
	wndclass.lpfnWndProc = WndProc;
	wndclass.cbClsExtra = 0;
	wndclass.cbWndExtra = 0;
	wndclass.hInstance = hInstance;
	wndclass.hIcon = LoadIcon(NULL, IDI_APPLICATION);
	wndclass.hCursor = LoadCursor(NULL, IDC_ARROW);
	wndclass.hbrBackground = (HBRUSH)GetStockObject(WHITE_BRUSH);
	wndclass.lpszMenuName = NULL;
	wndclass.lpszClassName = _T("Debug");
	wndclass.hIconSm = LoadIcon(NULL, IDI_APPLICATION);

	RegisterClassEx(&wndclass);

	hWnd = CreateWindow(_T("Debug"), _T("Debug"), WS_OVERLAPPEDWINDOW, 256, 256, 512, 256, NULL, NULL, hInstance, NULL);

	ShowWindow(hWnd, nCmdShow);
	UpdateWindow(hWnd);

	while (GetMessage(&msg, NULL, 0, 0))
	{
		TranslateMessage(&msg);
		DispatchMessage(&msg);
	}

	if (debug)
		delete debug;

	{
		LOCK(Lock_Instance);
		CleanUp();
	}

	WSACleanup();

	_RPT0(_CRT_WARN, "--- PROCESS_END ---\n");
//	CloseHandle(hLogFile);

	return (int)msg.wParam;
}
#else
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
{
	if (Init(hInstance) != 0)
		return -1;

	WSADATA wsa;
	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
		return -2;

	int ret = Listen(g_Host, g_Port);

	{
		// 来ないけど一応
		LOCK(Lock_Instance);
		CleanUp();
	}

	WSACleanup();
	return ret;
}
#endif
