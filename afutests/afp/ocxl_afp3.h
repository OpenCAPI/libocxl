/*
 * Copyright 2018 International Business Machines
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef _OCXL_AFP3_H
#define _OCXL_AFP3_H

// global mmio registers
#define AFUConfiguration_REGISTER     0x0000
#define AFUInternalError_REGISTER     0x0010
#define AFUInternalErrorInfo_REGISTER 0x0018
#define AFUTraceControl_REGISTER      0x0020

// global AFP3 registers
#define AFUExtraReadEA_AFP_REGISTER  0x0038
#define AFUWED_AFP_REGISTER          0x0040
#define AFUBufmask_AFP_REGISTER      0x0048
#define AFUPASID_AFP_REGISTER        0x0050
#define AFUMisc_AFP_REGISTER         0x0058
#define AFUEnable_AFP_REGISTER       0x0060
#define AFUControl_AFP_REGISTER      0x0068
#define AFULatency_AFP_REGISTER      0x0070
#define AFUPerfCnt0_AFP_REGISTER     0x00C0
#define AFUPerfCnt1_AFP_REGISTER     0x00C8
#define AFUPerfCnt2_AFP_REGISTER     0x00D0
#define AFUPerfCnt3_AFP_REGISTER     0x00D8
#define AFUPerfCnt4_AFP_REGISTER     0x00E0
#define AFUPerfCnt5_AFP_REGISTER     0x00E8
#define AFUPerfCnt6_AFP_REGISTER     0x00F0
#define AFUPerfCnt7_AFP_REGISTER     0x00F8
#define Large_Data0_AFP_REGISTER     0x10000
#define Large_Data1_AFP_REGISTER     0x10080
#define Large_Data2_AFP_REGISTER     0x10100
#define Large_Data3_AFP_REGISTER     0x10180

#endif /* _OCXL_AFP3_H */
